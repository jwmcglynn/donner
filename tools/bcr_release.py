#!/usr/bin/env python3
"""Resolve verified CI artifacts and guard automatic BCR submission."""

from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlencode

from tools import bcr_source
from tools import release_artifact_publisher


REPOSITORY = "jwmcglynn/donner"
REGISTRY_FORK = "jwmcglynn/bazel-central-registry"
PREFLIGHT_PATH = ".github/workflows/bcr_preflight.yml"
RELEASE_PATH = ".github/workflows/release.yml"


def gh_json(*args: str):
    return json.loads(subprocess.check_output(["gh", *args], text=True))


def check_run(run: dict, commit: str, path: str, events: set[str]) -> None:
    if (run.get("head_sha") != commit or run.get("path") != path
            or run.get("event") not in events or run.get("status") != "completed"
            or run.get("conclusion") != "success"
            or run.get("repository", {}).get("full_name") != REPOSITORY):
        raise ValueError("workflow run is not a successful build of the approved source")
    if type(run.get("run_attempt")) is not int or run["run_attempt"] < 1:
        raise ValueError("workflow run has an invalid attempt")


def check_source(commit: str, tag: str) -> str:
    if not bcr_source.COMMIT.fullmatch(commit):
        raise ValueError("release source must be a full Git commit ID")
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    if tag != f"v{version}":
        raise ValueError("release tag does not match the module version")
    subprocess.run(["git", "merge-base", "--is-ancestor", commit, "origin/main"], check=True)
    release_artifact_publisher.verify_remote_tag(tag, commit)
    return version


def select_preflight(commit: str, tag: str) -> dict[str, object]:
    version = check_source(commit, tag)
    query = urlencode({"head_sha": commit, "per_page": 100})
    runs = gh_json("api", f"repos/{REPOSITORY}/actions/workflows/bcr_preflight.yml/runs?{query}")
    eligible = [run for run in runs["workflow_runs"]
                if run.get("event") in {"push", "workflow_dispatch"}]
    if not eligible:
        raise ValueError("run BCR Preflight for this exact commit before publishing")
    selected = max(eligible, key=lambda run: run["id"])
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{selected['id']}")
    check_run(run, commit, PREFLIGHT_PATH, {"push", "workflow_dispatch"})
    return {"run_id": str(run["id"]), "attempt": str(run["run_attempt"]), "version": version,
            "artifact": f"donner-bcr-qualified-{run['run_attempt']}"}


def check_release(release: dict, tag: str) -> None:
    if (release.get("tagName") != tag or release.get("isDraft") is not False
            or type(release.get("isPrerelease")) is not bool):
        raise ValueError("expected an existing published release")


def verify_published_source(commit: str, tag: str, release: dict) -> dict:
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    prefix = f"donner-{version}"
    names = {f"{prefix}.tar.gz", f"{prefix}.tar.gz.sha256", f"{prefix}.provenance.json"}
    assets = {asset["name"]: asset for asset in release["assets"]}
    if not names.issubset(assets):
        raise ValueError("verified source release assets are missing")
    if any(sum(asset.get("name") == name for asset in release["assets"]) != 1 for name in names):
        raise ValueError("source release assets have duplicate names")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for name in sorted(names):
            subprocess.run(["gh", "release", "download", tag, "--repo", REPOSITORY,
                            "--pattern", name, "--dir", str(directory)], check=True)
        receipt = bcr_source.verify_archive(directory, commit)
        if assets[f"{prefix}.tar.gz"].get("digest") != "sha256:" + receipt["sha256"]:
            raise ValueError("published source digest does not match its verified bytes")
    run_id = receipt.get("verified_run_id", "")
    attempt = receipt.get("verified_run_attempt", "")
    if not re.fullmatch(r"[1-9][0-9]*", str(run_id)) or not re.fullmatch(r"[1-9][0-9]*", str(attempt)):
        raise ValueError("source provenance is missing its CI run and attempt")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    check_run(run, commit, PREFLIGHT_PATH, {"push", "workflow_dispatch"})
    if run["run_attempt"] != int(attempt):
        raise ValueError("source CI attempt does not match provenance")
    return receipt


def fork_contents(path: str, commit: str) -> bytes:
    value = gh_json("api", f"repos/{REGISTRY_FORK}/contents/{path}?ref={commit}")
    return base64.b64decode(value["content"], validate=False)


def existing_submission(tag: str, commit: str, receipt: dict) -> str | None:
    branch = f"donner-{tag}"
    refs = gh_json("api", f"repos/{REGISTRY_FORK}/git/matching-refs/heads/{branch}")
    matches = [ref for ref in refs if ref["ref"] == f"refs/heads/{branch}"]
    if not matches:
        return None
    if len(matches) != 1:
        raise ValueError("ambiguous registry submission branch")
    head = matches[0]["object"]["sha"]
    if not bcr_source.COMMIT.fullmatch(head):
        raise ValueError("invalid registry submission commit")
    root = f"modules/donner/{receipt['version']}"
    source = json.loads(fork_contents(f"{root}/source.json", head))
    expected = {
        "integrity": "sha256-" + base64.b64encode(bytes.fromhex(receipt["sha256"])).decode(),
        "strip_prefix": f"donner-{receipt['version']}",
        "url": bcr_source.source_url(receipt["version"]),
    }
    if source != expected:
        raise ValueError("existing registry branch has different source metadata; inspect it manually")
    for file, original in [("MODULE.bazel", "MODULE.bazel"), ("presubmit.yml", ".bcr/presubmit.yml")]:
        expected_bytes = subprocess.check_output(["git", "show", f"{commit}:{original}"])
        if fork_contents(f"{root}/{file}", head) != expected_bytes:
            raise ValueError("existing registry branch has different module or test files")
    query = urlencode({"head": f"jwmcglynn:{branch}", "state": "all"})
    prs = gh_json("api", f"repos/bazelbuild/bazel-central-registry/pulls?{query}")
    matching = [pr for pr in prs if pr["head"]["sha"] == head
                and pr["head"].get("repo", {}).get("full_name") == REGISTRY_FORK
                and (pr["state"] == "open" or pr.get("merged_at"))]
    if len(matching) != 1:
        raise ValueError("existing registry branch needs manual PR recovery; it will not be overwritten")
    return matching[0]["html_url"]


def plan_submission(release_run_id: str) -> dict[str, object]:
    if not re.fullmatch(r"[1-9][0-9]*", release_run_id):
        raise ValueError("release workflow run ID must be numeric")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{release_run_id}")
    commit = run.get("head_sha", "")
    check_run(run, commit, RELEASE_PATH, {"release"})
    if not bcr_source.COMMIT.fullmatch(commit):
        raise ValueError("invalid release source commit")
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    tag = f"v{version}"
    release = gh_json("release", "view", tag, "--repo", REPOSITORY,
                      "--json", "tagName,isDraft,isPrerelease,assets")
    check_release(release, tag)
    if release.get("isPrerelease") or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        return {"publish": "false", "reason": "BCR submission is limited to stable releases"}
    check_source(commit, tag)
    receipt = verify_published_source(commit, tag, release)
    existing = existing_submission(tag, commit, receipt)
    return {"publish": "false" if existing else "true", "tag": tag,
            "existing_pr": existing, "source_commit": commit}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select-preflight")
    select.add_argument("--commit", required=True)
    select.add_argument("--tag", required=True)
    select.add_argument("--github-output")
    plan = commands.add_parser("plan-submission")
    plan.add_argument("--release-run-id", required=True)
    plan.add_argument("--github-output")
    args = parser.parse_args()
    if args.command == "select-preflight":
        result = select_preflight(args.commit, args.tag)
    else:
        result = plan_submission(args.release_run_id)
    bcr_source.outputs(result, args.github_output)


if __name__ == "__main__":
    main()
