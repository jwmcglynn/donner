"""Bounded, content-addressed documentation storage on a data-only Git branch."""
from __future__ import annotations

import base64
import gzip
import hashlib
import io
import math
import os
import json
from pathlib import Path
import subprocess
import tempfile
from urllib.parse import quote

from tools import docs_site as site

REPO = "repos/jwmcglynn/donner"
BRANCH = "docs-snapshots"
CHUNK = 40 * 1024 * 1024
MAX_CONTROL = 64 * 1024 * 1024
RAW_CHUNK = 16 * 1024 * 1024


def automatic_context() -> None:
    site.require(os.environ.get("GITHUB_REPOSITORY") == "jwmcglynn/donner"
                 and os.environ.get("GITHUB_REF") == "refs/heads/main"
                 and os.environ.get("GITHUB_EVENT_NAME") == "workflow_run"
                 and os.environ.get("GITHUB_WORKFLOW_REF") ==
                     "jwmcglynn/donner/.github/workflows/deploy_docs.yaml@refs/heads/main"
                 and bool(site.SHA.fullmatch(os.environ.get("GITHUB_SHA", ""))),
                 "automatic docs require the trusted main workflow-run context")


def deployment_environment() -> str:
    if os.environ.get("GITHUB_EVENT_NAME") == "workflow_run":
        automatic_context()
        return "github-pages-main"
    return "github-pages"


class GitHub:
    def call(self, endpoint: str, method: str = "GET", data: dict | None = None,
             raw: bool = False, maximum: int = MAX_CONTROL) -> object:
        args = ["gh", "api", endpoint, "--method", method]
        if raw:
            args += ["-H", "Accept: application/vnd.github.raw+json"]
        with tempfile.TemporaryFile() as output:
            try:
                result = subprocess.run(args + (["--input", "-"] if data is not None else []),
                                        input=site.canonical(data) if data is not None else None,
                                        stdout=output, stderr=subprocess.PIPE, timeout=180, check=False)
            except (OSError, subprocess.SubprocessError):
                raise ValueError("GitHub request outcome is unknown: " + endpoint.split("?")[0]) from None
            if result.returncode:
                raise ValueError("GitHub request failed: " + endpoint.split("?")[0]
                                 + (" (404)" if b"HTTP 404" in result.stderr else ""))
            site.require(output.tell() <= maximum, "GitHub response is oversized")
            output.seek(0)
            payload = output.read(maximum + 1)
        return payload if raw else json.loads(payload)

    def tip(self) -> str | None:
        try:
            value = self.call(REPO + "/git/ref/heads/" + BRANCH)
        except ValueError as error:
            if str(error).endswith("(404)"):
                return None
            raise
        sha = value["object"]["sha"]
        site.require(site.SHA.fullmatch(sha), "invalid snapshot commit")
        return sha

    def require_protection(self) -> None:
        rules = self.call(REPO + "/rules/branches/" + BRANCH)
        site.require(isinstance(rules, list) and {"deletion", "non_fast_forward"}
                     <= {rule.get("type") for rule in rules},
                     "docs-snapshots needs active deletion and non-fast-forward protection")
        name = deployment_environment()
        environment = self.call(REPO + "/environments/" + name)
        reviewers = [rule for rule in environment.get("protection_rules", [])
                     if rule.get("type") == "required_reviewers" and rule.get("reviewers")]
        if name == "github-pages":
            site.require(isinstance(environment.get("can_admins_bypass"), bool) and bool(reviewers),
                         "github-pages needs required reviewers and an explicit administrator bypass setting")
        else:
            site.require(isinstance(environment.get("can_admins_bypass"), bool) and not reviewers,
                         "github-pages-main needs automatic approval and an explicit administrator bypass setting")
        policy = environment.get("deployment_branch_policy") or {}
        site.require(policy == {"protected_branches": False, "custom_branch_policies": True},
                     "github-pages needs an explicit main-only branch policy")
        branches = self.call(REPO + "/environments/" + name + "/deployment-branch-policies")
        selected = branches.get("branch_policies", [])
        site.require(branches.get("total_count") == 1 and len(selected) == 1
                     and selected[0].get("name") == "main" and selected[0].get("type") == "branch",
                     "github-pages must allow only the main branch")

    def file(self, commit: str, name: str) -> bytes:
        site.require(site.SHA.fullmatch(commit), "invalid snapshot commit")
        site.safe_path(name)
        return self.call(REPO + "/contents/" + quote(name, safe="/") + "?ref=" + commit, raw=True)


def index(api: GitHub, expected: str | None) -> dict:
    site.require(api.tip() == expected, "snapshot branch advanced")
    if expected is None:
        return {"schema": 1, "releases": {}, "current": None, "previous": None}
    value = json.loads(api.file(expected, "index.json"))
    site.require(set(value) == {"schema", "releases", "current", "previous"}
                 and value["schema"] == 1 and isinstance(value["releases"], dict)
                 and len(value["releases"]) <= site.MAX_VERSIONS, "invalid snapshot index")
    for key in ("current", "previous"):
        site.require(value[key] is None or site.DIGEST.fullmatch(value[key]), "invalid composition ID")
    return value


def split_archive(archive: Path, payload: Path) -> dict:
    site.require(0 < archive.stat().st_size <= site.MAX_SITE, "archive exceeds storage limit")
    parts = []
    with archive.open("rb") as source:
        while raw := source.read(RAW_CHUNK):
            compressed = io.BytesIO()
            with gzip.GzipFile(fileobj=compressed, mode="wb", mtime=0) as zipper:
                zipper.write(raw)
            data = compressed.getvalue()
            site.require(len(data) <= CHUNK, "compressed chunk exceeds storage limit")
            compressed_sha = site.digest(data)
            name = f"objects/{compressed_sha}/0000.part"
            path = payload / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists():
                site.require(path.read_bytes() == data, "stored chunk collision")
            else:
                path.write_bytes(data)
            parts.append({"path": name, "sha256": compressed_sha, "size": len(data),
                          "raw_sha256": site.digest(raw), "raw_size": len(raw)})
    return {"schema": 2, "sha256": _file_digest(archive), "size": archive.stat().st_size,
            "chunks": parts}


def _file_digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _validate_archive_v1(record: dict) -> None:
    site.require(set(record) == {"sha256", "size", "compressed_sha256", "chunks"},
                 "unexpected stored archive fields")
    site.require(site.DIGEST.fullmatch(record["sha256"])
                 and site.DIGEST.fullmatch(record["compressed_sha256"])
                 and type(record["size"]) is int and 0 < record["size"] <= site.MAX_SITE,
                 "invalid stored archive")
    site.require(isinstance(record["chunks"], list) and 0 < len(record["chunks"]) <= 32,
                 "invalid stored chunks")
    for number, part in enumerate(record["chunks"]):
        name = f"objects/{record['compressed_sha256']}/{number:04}.part"
        site.require(set(part) == {"path", "sha256", "size"} and part["path"] == name
                     and site.DIGEST.fullmatch(part["sha256"])
                     and type(part["size"]) is int and 0 < part["size"] <= CHUNK,
                     "invalid stored chunk")


def validate_archive(record: dict) -> None:
    if "schema" not in record:
        _validate_archive_v1(record)
        return
    site.require(set(record) == {"schema", "sha256", "size", "chunks"} and record["schema"] == 2,
                 "unexpected stored archive format")
    site.require(bool(site.DIGEST.fullmatch(record["sha256"])) and type(record["size"]) is int
                 and 0 < record["size"] <= site.MAX_SITE and isinstance(record["chunks"], list)
                 and 0 < len(record["chunks"]) <= math.ceil(site.MAX_SITE / RAW_CHUNK),
                 "invalid stored archive")
    for part in record["chunks"]:
        site.require(set(part) == {"path", "sha256", "size", "raw_sha256", "raw_size"}
                     and bool(site.DIGEST.fullmatch(part["sha256"]))
                     and part["path"] == f"objects/{part['sha256']}/0000.part"
                     and bool(site.DIGEST.fullmatch(part["raw_sha256"]))
                     and type(part["size"]) is int and 0 < part["size"] <= CHUNK
                     and type(part["raw_size"]) is int and 0 < part["raw_size"] <= RAW_CHUNK,
                     "invalid stored chunk")
    site.require(sum(part["raw_size"] for part in record["chunks"]) == record["size"],
                 "stored raw chunk sizes differ")


def restore(record: dict, output: Path, read) -> None:
    validate_archive(record)
    if "schema" not in record:
        _restore_v1(record, output, read)
        return
    site.require(not output.exists(), "restore output already exists")
    with output.open("xb") as destination:
        for part in record["chunks"]:
            data = read(part["path"])
            site.require(len(data) == part["size"] and site.digest(data) == part["sha256"],
                         "stored chunk changed")
            with gzip.GzipFile(fileobj=io.BytesIO(data)) as source:
                raw = source.read(part["raw_size"] + 1)
            site.require(len(raw) == part["raw_size"] and site.digest(raw) == part["raw_sha256"],
                         "stored raw chunk changed")
            destination.write(raw)
    site.require(output.stat().st_size == record["size"] and _file_digest(output) == record["sha256"],
                 "restored archive changed")


def _restore_v1(record: dict, output: Path, read) -> None:
    validate_archive(record)
    site.require(not output.exists(), "restore output already exists")
    with tempfile.TemporaryFile() as compressed:
        for part in record["chunks"]:
            data = read(part["path"])
            site.require(len(data) == part["size"] and site.digest(data) == part["sha256"],
                         "stored chunk changed")
            compressed.write(data)
        compressed.seek(0)
        site.require(hashlib.file_digest(compressed, "sha256").hexdigest() == record["compressed_sha256"],
                     "compressed archive changed")
        compressed.seek(0)
        with gzip.GzipFile(fileobj=compressed) as source, output.open("xb") as destination:
            total = 0
            while block := source.read(1024 * 1024):
                total += len(block)
                site.require(total <= record["size"], "stored archive expands beyond its limit")
                destination.write(block)
    site.require(output.stat().st_size == record["size"] and _file_digest(output) == record["sha256"],
                 "restored archive changed")


def commit(api: GitHub, expected: str | None, payload: Path, new_index: dict) -> str:
    site.require(api.tip() == expected, "snapshot branch advanced before write")
    api.require_protection()
    entries = []
    for path in sorted(payload.rglob("*")):
        if path.is_dir():
            continue
        site.require(not path.is_symlink() and path.stat().st_size <= CHUNK, "unsafe storage payload")
        name = site.safe_path(path.relative_to(payload).as_posix())
        site.require(name.startswith(("objects/", "compositions/", "releases/")), "invalid store namespace")
        if expected is not None:
            try:
                previous = api.file(expected, name)
            except ValueError as error:
                site.require(str(error).endswith("(404)"), "cannot inspect existing storage object")
            else:
                site.require(previous == path.read_bytes(), "immutable storage entry would change")
                continue
        blob = api.call(REPO + "/git/blobs", "POST", {
            "encoding": "base64", "content": base64.b64encode(path.read_bytes()).decode()})
        entries.append({"path": name, "mode": "100644", "type": "blob", "sha": blob["sha"]})
    blob = api.call(REPO + "/git/blobs", "POST", {"encoding": "utf-8", "content": site.canonical(new_index).decode()})
    entries.append({"path": "index.json", "mode": "100644", "type": "blob", "sha": blob["sha"]})
    tree_data = {"tree": entries}
    if expected:
        tree_data["base_tree"] = api.call(REPO + "/git/commits/" + expected)["tree"]["sha"]
    tree = api.call(REPO + "/git/trees", "POST", tree_data)
    created = api.call(REPO + "/git/commits", "POST", {
        "message": "Retain verified documentation composition", "tree": tree["sha"],
        "parents": [expected] if expected else []})
    site.require(api.tip() == expected, "snapshot branch advanced before activation")
    if expected:
        api.call(REPO + "/git/refs/heads/" + BRANCH, "PATCH", {"sha": created["sha"], "force": False})
    else:
        api.call(REPO + "/git/refs", "POST", {"ref": "refs/heads/" + BRANCH, "sha": created["sha"]})
    site.require(api.tip() == created["sha"], "snapshot activation is unconfirmed")
    return created["sha"]
