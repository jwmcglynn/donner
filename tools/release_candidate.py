#!/usr/bin/env python3
"""Verify an immutable Donner release-candidate record against retained bytes and GitHub."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import zipfile

from tools import bcr_source, pages_artifact, release_cli


REPOSITORY = "jwmcglynn/donner"
ARTIFACT_WORKFLOWS = {
    "source": ".github/workflows/bcr_preflight.yml",
    "cli-linux": ".github/workflows/bcr_preflight.yml",
    "cli-macos": ".github/workflows/bcr_preflight.yml",
    "editor": ".github/workflows/editor_wasm.yml",
    "docs": ".github/workflows/deploy_docs.yaml",
}
EVIDENCE_WORKFLOWS = {
    "ci": ".github/workflows/main.yml",
    "coverage": ".github/workflows/coverage.yml",
    "fuzz": ".github/workflows/fuzz.yml",
    "sanitizers": ".github/workflows/sanitizers.yml",
    "security": ".github/workflows/codeql.yml",
}
REQUIRED_EVIDENCE_JOBS = {
    "ci": (("linux", "linux-self-hosted"), ("macos", "macos-self-hosted")),
    "coverage": (("build", "coverage-self-hosted"),),
    "fuzz": (("linux",), ("macos",)),
    "sanitizers": (("asan",), ("ubsan",)),
    "security": (("Analyze (c-cpp)",), ("Analyze (javascript-typescript)",),
                 ("Analyze (python)",)),
}
REVIEW_NAMES = {"security", "supply-chain"}
SHA = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
MAX_ZIP_BYTES = 1_000_000_000
MAX_UNPACKED_BYTES = 2_000_000_000
MAX_FILES = 100_000
MAX_RECORD_BYTES = 10_000_000
MAX_SMALL_CONTROL_BYTES = 1_000_000
MAX_LOCK_BYTES = 16_000_000
EDITOR_COMMON_FILES = {
    "CatalogFontNotices.txt", "catalog-fonts.js", "catalog-fonts.json", "donner_icon.svg",
    "editor-bootstrap.js", "editor.css", "editor.js", "enable-threads.js", "index.html",
}


def _canonical_bytes(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def _sha256(path: Path) -> str:
    if not path.is_file() or path.is_symlink():
        raise ValueError("candidate input must be a regular file")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def _positive(value: object, label: str) -> int:
    if type(value) is not int or value < 1 or value > 9_007_199_254_740_991:
        raise ValueError(f"{label} must be a canonical positive integer")
    return value


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def _commit(value: object, label: str) -> str:
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ValueError(f"{label} must be a full lowercase Git commit")
    return value


def _record_shape(record: dict) -> None:
    if set(record) != {"schema", "source", "artifacts", "evidence", "reviews"} or record["schema"] != 1:
        raise ValueError("candidate record has an unsupported schema")
    if any(not isinstance(record[key], dict) for key in ("source", "artifacts", "evidence", "reviews")):
        raise ValueError("candidate record sections must be objects")
    if set(record["artifacts"]) != set(ARTIFACT_WORKFLOWS):
        raise ValueError("candidate record must name every publishable artifact role")
    if set(record["evidence"]) != set(EVIDENCE_WORKFLOWS):
        raise ValueError("candidate record must name every qualification evidence role")
    if set(record["reviews"]) != REVIEW_NAMES:
        raise ValueError("candidate record must bind security and supply-chain reviews")


def _verify_source(source: dict, root: Path) -> tuple[str, str]:
    if set(source) != {"commit", "tree", "version", "compatibility_level", "bazel_version",
                       "module_locks_sha256", "compiler_versions"}:
        raise ValueError("candidate source fields are incomplete")
    commit = _commit(source["commit"], "source commit")
    tree = _commit(source["tree"], "source tree")
    if _git(root, "rev-parse", "HEAD") != commit or _git(root, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate verification needs a clean exact-source checkout")
    if _git(root, "rev-parse", "HEAD^{tree}") != tree:
        raise ValueError("candidate source tree does not match its commit")
    values = bcr_source.module_values((root / "MODULE.bazel").read_text(encoding="utf-8"))
    if (type(source["compatibility_level"]) is not int or
            source["version"] != values["version"] or
            source["compatibility_level"] != values.get("compatibility_level") or
            source["bazel_version"] != (root / ".bazelversion").read_text(encoding="utf-8").strip()):
        raise ValueError("candidate module or toolchain metadata differs from source")
    locks = source["module_locks_sha256"]
    compilers = source["compiler_versions"]
    if (not isinstance(locks, dict) or not isinstance(compilers, dict) or
            set(locks) != {"cli-linux", "cli-macos"} or
            set(compilers) != {"cli-linux", "cli-macos"}):
        raise ValueError("candidate needs per-platform lock and compiler records")
    for role in ("cli-linux", "cli-macos"):
        _digest(locks[role], "module lock digest")
        if not isinstance(compilers[role], str) or not re.fullmatch(r"[A-Za-z0-9 .+()~_-]{1,256}", compilers[role]):
            raise ValueError("candidate compiler version is invalid")
    return commit, tree


def _github_json(*args: str) -> dict:
    return json.loads(subprocess.check_output(["gh", "api", *args], text=True))


def _verify_run(run: dict, run_id: int, attempt: int, commit: str, workflow: str) -> None:
    if (run.get("id") != run_id or run.get("run_attempt") != attempt or
            run.get("repository", {}).get("full_name") != REPOSITORY or
            run.get("path") != workflow or run.get("head_sha") != commit or
            run.get("head_branch") != "main" or
            run.get("event") not in ("push", "workflow_dispatch", "schedule") or
            run.get("status") != "completed" or run.get("conclusion") != "success"):
        raise ValueError("candidate evidence run is not a successful exact-source attempt")
    if workflow == ARTIFACT_WORKFLOWS["docs"] and (run.get("event") != "push" or run.get("head_branch") != "main"):
        raise ValueError("documentation artifact must come from a main push")


def _verify_artifact_metadata(artifact: dict, role: str, entry: dict, commit: str,
                              run_record: dict) -> None:
    run = artifact.get("workflow_run") or {}
    repository_id = run_record.get("repository", {}).get("id")
    if (artifact.get("id") != entry["artifact_id"] or artifact.get("name") != entry["name"] or
            artifact.get("digest") != "sha256:" + entry["zip_sha256"] or
            artifact.get("expired") is not False or
            type(artifact.get("size_in_bytes")) is not int or
            not 0 < artifact["size_in_bytes"] <= MAX_ZIP_BYTES or
            run.get("id") != entry["run_id"] or run.get("head_sha") != commit or
            run.get("head_branch") != "main" or
            repository_id is None or run.get("repository_id") != repository_id or
            run.get("head_repository_id") != repository_id):
        raise ValueError(f"{role} artifact metadata does not match the selected retained bytes")


def _artifact_name(role: str, commit: str, attempt: int) -> str:
    return {
        "source": f"donner-bcr-qualified-{attempt}",
        "cli-linux": f"donner-svg-linux-x86-64-{commit}-{attempt}",
        "cli-macos": f"donner-svg-darwin-arm64-{commit}-{attempt}",
        "editor": f"donner-editor-wasm-geode-{commit}",
        "docs": "github-pages",
    }[role]


def _safe_zip_path(name: str) -> PurePosixPath:
    normalized = name.removesuffix("/")
    path = PurePosixPath(normalized)
    if (not name or len(name) > 256 or not name.isascii() or
            not re.fullmatch(r"[A-Za-z0-9._/-]+", normalized) or
            "\\" in name or path.is_absolute() or
            any(part in ("", ".", "..") for part in normalized.split("/"))):
        raise ValueError("candidate artifact ZIP contains an unsafe path")
    return path


def _fixed_role_files(role: str, version: str) -> set[str] | None:
    if role == "source":
        archive = f"donner-{version}.tar.gz"
        return {archive, f"{archive}.sha256", f"donner-{version}.provenance.json"}
    if role in ("cli-linux", "cli-macos"):
        platform = "linux-x86-64" if role == "cli-linux" else "darwin-arm64"
        return set(release_cli._names(platform))
    if role == "docs":
        return {"artifact.tar"}
    return None


def _editor_font_files(names: set[str]) -> set[str]:
    fonts = {name.removeprefix("site/") for name in names if name.startswith("site/fonts/")}
    if (any(not re.fullmatch(r"fonts/[0-9a-f]{64}\.woff2", font) for font in fonts) or
            {f"deploy/{font}" for font in fonts} !=
            {name for name in names if name.startswith("deploy/fonts/")}):
        raise ValueError("editor font paths are outside the published catalog shape")
    return {f"{prefix}/{font}" for prefix in ("site", "deploy") for font in fonts}


def _verify_role_files(role: str, names: set[str], version: str) -> None:
    required = _fixed_role_files(role, version)
    if required is None:
        required = {"provenance.json", "SHA256SUMS", "site/editor.wasm",
                    "deploy/editor.wasm.gz"}
        required.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                        for name in EDITOR_COMMON_FILES)
        required.update(_editor_font_files(names))
    if names != required:
        raise ValueError("candidate artifact inventory differs from its publishable role")


def _member_size_limit(name: str) -> int:
    if name in ("SHA256SUMS", "provenance.json") or name.endswith(
        (".provenance", ".provenance.json", ".sha256")
    ):
        return MAX_SMALL_CONTROL_BYTES
    if name.endswith(".bazel.lock"):
        return MAX_LOCK_BYTES
    return MAX_UNPACKED_BYTES


def _copy_checked_entry(archive: zipfile.ZipFile, item: zipfile.ZipInfo,
                        expected_digest: str, output: Path) -> None:
    target = output / item.filename
    target.parent.mkdir(parents=True, exist_ok=True)
    checksum = hashlib.sha256()
    with archive.open(item) as source, target.open("xb") as destination:
        while chunk := source.read(1024 * 1024):
            checksum.update(chunk)
            destination.write(chunk)
    if checksum.hexdigest() != expected_digest:
        raise ValueError("candidate artifact member digest differs")


def _bounded_members(archive: zipfile.ZipFile) -> list[zipfile.ZipInfo]:
    members = archive.infolist()
    if len(members) > MAX_FILES:
        raise ValueError("candidate artifact ZIP has too many entries")
    return members


def _extract_verified(zip_path: Path, expected: dict[str, str], output: Path) -> None:
    if not zip_path.is_file() or zip_path.is_symlink() or zip_path.stat().st_size > MAX_ZIP_BYTES:
        raise ValueError("candidate artifact ZIP is missing, linked, or too large")
    if not expected or len(expected) > MAX_FILES:
        raise ValueError("candidate artifact file inventory is missing or too large")
    for name, digest in expected.items():
        _safe_zip_path(name)
        _digest(digest, "artifact member digest")
    total = 0
    seen: set[str] = set()
    with zipfile.ZipFile(zip_path) as archive:
        for item in _bounded_members(archive):
            name = item.filename
            _safe_zip_path(name)
            if item.is_dir():
                continue
            if name in seen or name not in expected:
                raise ValueError("candidate artifact ZIP has duplicate or unexpected files")
            mode = item.external_attr >> 16
            if (stat.S_IFMT(mode) not in (0, stat.S_IFREG) or
                    item.file_size > MAX_UNPACKED_BYTES - total or
                    item.file_size > _member_size_limit(name)):
                raise ValueError("candidate artifact ZIP has a link or exceeds the byte budget")
            total += item.file_size
            seen.add(name)
            _copy_checked_entry(archive, item, expected[name], output)
    if seen != set(expected):
        raise ValueError("candidate artifact ZIP is missing retained files")


def _verify_editor(directory: Path, commit: str, root: Path, expected: dict[str, str],
                   run_id: str, attempt: str) -> None:
    provenance = json.loads((directory / "provenance.json").read_text(encoding="utf-8"))
    required_keys = {"source_revision", "producer_run_id", "producer_attempt",
                     "lockfile_sha256", "bazel_configs", "targets", "renderer",
                     "transport_encoding"}
    if (not isinstance(provenance, dict) or set(provenance) != required_keys or
            provenance.get("source_revision") != commit or provenance.get("renderer") != "geode" or
            provenance.get("producer_run_id") != run_id or
            provenance.get("producer_attempt") != attempt or
            provenance.get("bazel_configs") != ["editor-wasm"] or
            provenance.get("transport_encoding") != "gzip" or
            not isinstance(provenance.get("targets"), list) or len(provenance["targets"]) != 1 or
            not isinstance(provenance["targets"][0], str) or
            not re.fullmatch(r"//[A-Za-z0-9_./-]+:[A-Za-z0-9_./-]+", provenance["targets"][0]) or
            provenance.get("lockfile_sha256") != _sha256(root / "donner/editor/wasm/tests/package-lock.json")):
        raise ValueError("editor package provenance differs from the candidate")
    required = {"provenance.json", "SHA256SUMS", "site/index.html", "site/editor.wasm",
                "site/catalog-fonts.json", "deploy/index.html", "deploy/editor.wasm.gz"}
    if not required <= set(expected):
        raise ValueError("editor package is missing required site or deploy files")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        digest, separator, name = line.partition("  ")
        if separator != "  " or name in checksums:
            raise ValueError("editor package has an invalid checksum manifest")
        checksums[name] = _digest(digest, "editor file digest")
    if checksums != {name: sha for name, sha in expected.items() if name.startswith(("site/", "deploy/"))}:
        raise ValueError("editor package checksum manifest differs from retained files")


def _verify_payload(role: str, directory: Path, entry: dict, record: dict, root: Path) -> None:
    commit = record["source"]["commit"]
    run_id, attempt = str(entry["run_id"]), str(entry["attempt"])
    if role == "source":
        receipt = bcr_source.verify_archive(directory, commit, run_id, attempt)
        if receipt["sha256"] != entry["files"][receipt["archive"]]:
            raise ValueError("source archive digest differs from candidate")
    elif role in ("cli-linux", "cli-macos"):
        platform = "linux-x86-64" if role == "cli-linux" else "darwin-arm64"
        provenance = release_cli.verify(root=root, artifacts=directory, platform=platform,
                                        commit=commit, run_id=run_id, attempt=attempt)
        if (provenance["generated_lock_sha256"] != record["source"]["module_locks_sha256"][role] or
                provenance["host_cxx_version"] != record["source"]["compiler_versions"][role]):
            raise ValueError("CLI generated lock differs from candidate dependency state")
    elif role == "editor":
        _verify_editor(directory, commit, root, entry["files"], run_id, attempt)
    else:
        selected = {"artifact_id": entry["artifact_id"], "run_id": entry["run_id"],
                    "attempt": entry["attempt"], "source_commit": commit,
                    "artifact_digest": "sha256:" + entry["zip_sha256"],
                    "archive_sha256": entry["files"]["artifact.tar"]}
        pages_artifact.verify_archive(directory / "artifact.tar", selected["archive_sha256"], selected)


def _verify_job_counts(response: dict, kind: str, passed: int, skipped: int) -> None:
    jobs = response.get("jobs")
    if (not isinstance(jobs, list) or type(response.get("total_count")) is not int or
            len(jobs) != response["total_count"] or
            any(job.get("conclusion") not in ("success", "skipped") for job in jobs) or
            sum(job["conclusion"] == "success" for job in jobs) != passed or
            sum(job["conclusion"] == "skipped" for job in jobs) != skipped):
        raise ValueError("candidate workflow job counts are incomplete or not green")
    successful = {job.get("name") for job in jobs if job["conclusion"] == "success"}
    if any(not successful.intersection(alternatives) for alternatives in REQUIRED_EVIDENCE_JOBS[kind]):
        raise ValueError("candidate qualification run skipped a required execution lane")


def _verify_one_artifact(role: str, entry: dict, record: dict, root: Path,
                         artifacts: Path, github_json) -> tuple[int, int]:
    if not isinstance(entry, dict) or set(entry) != {
        "run_id", "attempt", "artifact_id", "name", "zip_sha256", "files"
    } or not isinstance(entry["files"], dict):
        raise ValueError(f"{role} artifact record is incomplete")
    run_id = _positive(entry["run_id"], "artifact run ID")
    attempt = _positive(entry["attempt"], "artifact run attempt")
    _positive(entry["artifact_id"], "artifact ID")
    _digest(entry["zip_sha256"], "artifact ZIP digest")
    commit = record["source"]["commit"]
    _verify_role_files(role, set(entry["files"]), record["source"]["version"])
    if entry["name"] != _artifact_name(role, commit, attempt):
        raise ValueError("artifact name is invalid")
    run = github_json(f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    _verify_run(run, run_id, attempt, commit, ARTIFACT_WORKFLOWS[role])
    metadata = github_json(f"repos/{REPOSITORY}/actions/artifacts/{entry['artifact_id']}")
    _verify_artifact_metadata(metadata, role, entry, commit, run)
    if role == "docs":
        pages_artifact.verify_metadata(metadata, run, {
            "artifact_id": entry["artifact_id"], "run_id": run_id, "attempt": attempt,
            "source_commit": commit, "artifact_digest": "sha256:" + entry["zip_sha256"],
            "archive_sha256": entry["files"]["artifact.tar"],
        })
    zip_path = artifacts / f"{role}.zip"
    if not zip_path.is_file() or zip_path.is_symlink() or zip_path.stat().st_size > MAX_ZIP_BYTES:
        raise ValueError(f"{role} artifact ZIP is missing, linked, or too large")
    if _sha256(zip_path) != entry["zip_sha256"]:
        raise ValueError(f"{role} artifact ZIP digest differs from the candidate")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        _extract_verified(zip_path, entry["files"], directory)
        _verify_payload(role, directory, entry, record, root)
    return run_id, attempt


def _verify_artifacts(record: dict, root: Path, artifacts: Path, github_json) -> None:
    preflight: set[tuple[int, int]] = set()
    for role, entry in record["artifacts"].items():
        identity = _verify_one_artifact(role, entry, record, root, artifacts, github_json)
        if role in ("source", "cli-linux", "cli-macos"):
            preflight.add(identity)
    if len(preflight) != 1:
        raise ValueError("source and CLI artifacts must share one qualified preflight attempt")


def _verify_evidence(record: dict, evidence: Path, github_json) -> None:
    commit = record["source"]["commit"]
    for kind, entry in record["evidence"].items():
        if not isinstance(entry, dict) or set(entry) != {
            "run_id", "attempt", "passed_jobs", "skipped_jobs", "sha256"
        }:
            raise ValueError(f"{kind} qualification evidence is incomplete")
        run_id = _positive(entry["run_id"], "evidence run ID")
        attempt = _positive(entry["attempt"], "evidence run attempt")
        if (type(entry["passed_jobs"]) is not int or entry["passed_jobs"] < 0 or
                type(entry["skipped_jobs"]) is not int or entry["skipped_jobs"] < 0):
            raise ValueError("qualification workflow job counts are invalid")
        _digest(entry["sha256"], "evidence digest")
        run = github_json(f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
        _verify_run(run, run_id, attempt, commit, EVIDENCE_WORKFLOWS[kind])
        jobs = github_json(
            f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100")
        _verify_job_counts(jobs, kind, entry["passed_jobs"], entry["skipped_jobs"])
        if _sha256(evidence / f"{kind}.json") != entry["sha256"]:
            raise ValueError(f"{kind} evidence digest differs from the candidate")


def _verify_reviews(record: dict, reviews: Path) -> None:
    for kind, digest in record["reviews"].items():
        _digest(digest, "review report digest")
        if _sha256(reviews / f"{kind}.md") != digest:
            raise ValueError(f"{kind} review report digest differs from the candidate")


def verify(record: dict, root: Path, artifacts: Path, evidence: Path, reviews: Path,
           github_json=_github_json) -> None:
    _record_shape(record)
    _verify_source(record["source"], root)
    _verify_artifacts(record, root, artifacts, github_json)
    _verify_evidence(record, evidence, github_json)
    _verify_reviews(record, reviews)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--reviews", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.record.is_symlink() or args.record.stat().st_size > MAX_RECORD_BYTES:
            raise ValueError("candidate record is linked or too large")
        raw = args.record.read_bytes()
        record = json.loads(raw)
        if not isinstance(record, dict) or raw != _canonical_bytes(record):
            raise ValueError("candidate record is not canonical immutable JSON")
        verify(record, args.source_root, args.artifacts, args.evidence, args.reviews)
    except ValueError as error:
        raise SystemExit(f"Candidate verification failed: {error}") from None
    except (OSError, KeyError, TypeError, AttributeError, subprocess.CalledProcessError,
            zipfile.BadZipFile) as error:
        raise SystemExit(f"Candidate verification failed: {type(error).__name__}") from None
    print(f"Verified release candidate sha256:{hashlib.sha256(raw).hexdigest()}")


if __name__ == "__main__":
    main()
