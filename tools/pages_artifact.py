"""Validate a retained GitHub Pages artifact before manual publication."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tarfile


REPOSITORY = "jwmcglynn/donner"
WORKFLOW = ".github/workflows/deploy_docs.yaml"
ARTIFACT_NAME = "github-pages"
SHA = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
MAX_ARCHIVE_BYTES = 1_000_000_000
MAX_ARCHIVE_FILES = 100_000


def positive_id(value: str, label: str) -> int:
    if not value.isascii() or not value.isdecimal() or value.startswith("0") or len(value) > 16:
        raise ValueError(f"{label} must be a canonical positive decimal integer")
    number = int(value)
    if number > 9_007_199_254_740_991:
        raise ValueError(f"{label} exceeds the supported integer range")
    return number


def selection_from_environment() -> dict[str, object]:
    artifact_id = positive_id(os.environ["DOC_ARTIFACT_ID"], "artifact ID")
    run_id = positive_id(os.environ["DOC_SOURCE_RUN_ID"], "source run ID")
    attempt = positive_id(os.environ["DOC_SOURCE_ATTEMPT"], "source attempt")
    source_commit = os.environ["DOC_SOURCE_COMMIT"]
    artifact_digest = os.environ["DOC_ARTIFACT_DIGEST"]
    archive_sha256 = os.environ["DOC_ARCHIVE_SHA256"]
    if not SHA.fullmatch(source_commit):
        raise ValueError("source commit must be a full lowercase Git SHA")
    if not artifact_digest.startswith("sha256:") or not SHA256.fullmatch(
        artifact_digest.removeprefix("sha256:")
    ):
        raise ValueError("artifact digest must be sha256 followed by 64 lowercase hex digits")
    if not SHA256.fullmatch(archive_sha256):
        raise ValueError("archive digest must be 64 lowercase hex digits")
    return {
        "artifact_id": artifact_id,
        "run_id": run_id,
        "attempt": attempt,
        "source_commit": source_commit,
        "artifact_digest": artifact_digest,
        "archive_sha256": archive_sha256,
    }


def verify_source_run(run: dict, selected: dict[str, object]) -> None:
    repository = run.get("repository") or {}
    if repository.get("full_name") != REPOSITORY:
        raise ValueError("source run belongs to another repository")
    if run.get("id") != selected["run_id"] or run.get("run_attempt") != selected["attempt"]:
        raise ValueError("source run ID or attempt does not match")
    if (
        run.get("path") != WORKFLOW
        or run.get("event") != "push"
        or run.get("head_branch") != "main"
        or run.get("head_sha") != selected["source_commit"]
        or run.get("status") != "completed"
        or run.get("conclusion") != "success"
    ):
        raise ValueError("source run is not a successful main docs build for this commit")


def verify_metadata(artifact: dict, run: dict, selected: dict[str, object]) -> None:
    verify_source_run(run, selected)
    source = artifact.get("workflow_run") or {}
    repository = run["repository"]
    if artifact.get("id") != selected["artifact_id"] or artifact.get("name") != ARTIFACT_NAME:
        raise ValueError("selected Pages artifact ID or name does not match")
    if artifact.get("expired") is not False or not isinstance(artifact.get("size_in_bytes"), int):
        raise ValueError("selected Pages artifact is expired or has no size")
    if artifact["size_in_bytes"] < 1 or artifact["size_in_bytes"] > MAX_ARCHIVE_BYTES:
        raise ValueError("selected Pages artifact size is outside the accepted range")
    if artifact.get("digest") != selected["artifact_digest"]:
        raise ValueError("selected Pages artifact digest does not match")
    if (
        source.get("id") != selected["run_id"]
        or source.get("head_branch") != "main"
        or source.get("head_sha") != selected["source_commit"]
        or source.get("repository_id") != repository.get("id")
        or source.get("head_repository_id") != repository.get("id")
    ):
        raise ValueError("Pages artifact does not belong to the selected main run")


def verify_build_marker(archive: tarfile.TarFile, member: tarfile.TarInfo,
                        selected: dict[str, object]) -> None:
    if not member.isfile() or member.size > 1024:
        raise ValueError("Pages build marker is not a small regular file")
    with archive.extractfile(member) as stream:
        try:
            marker = json.loads(stream.read(1025))
        except (ValueError, UnicodeDecodeError) as error:
            raise ValueError("Pages build marker is malformed") from error
    expected = {
        "source_commit": selected["source_commit"],
        "source_run_id": selected["run_id"],
        "source_attempt": selected["attempt"],
    }
    if marker != expected:
        raise ValueError("Pages build marker does not match the selected source attempt")


def verify_archive_contents(path: Path, selected: dict[str, object]) -> None:
    seen: set[str] = set()
    total_bytes = 0
    found_index = False
    found_marker = False
    with tarfile.open(path, "r:") as archive:
        for member in archive:
            normalized = validate_member(member, seen)
            if len(seen) > MAX_ARCHIVE_FILES:
                raise ValueError("Pages archive contains too many members")
            total_bytes += member.size
            if total_bytes > MAX_ARCHIVE_BYTES:
                raise ValueError("Pages archive exceeds the uncompressed size limit")
            if str(normalized) == "index.html":
                if not member.isfile():
                    raise ValueError("Pages index.html is not a regular file")
                found_index = True
            elif str(normalized) == "docs-build.json":
                verify_build_marker(archive, member, selected)
                found_marker = True
    if not found_index:
        raise ValueError("Pages archive has no top-level index.html")
    if not found_marker:
        raise ValueError("Pages archive has no docs-build.json marker")


def validate_member(member: tarfile.TarInfo, seen: set[str]) -> PurePosixPath:
    name = member.name
    normalized = PurePosixPath(name)
    if (
        name.startswith("/")
        or "\\" in name
        or ".." in normalized.parts
        or str(normalized) in seen
        or not (member.isfile() or member.isdir())
    ):
        raise ValueError("Pages archive contains an unsafe or duplicate member")
    seen.add(str(normalized))
    return normalized


def verify_archive(path: Path, expected_sha256: str, selected: dict[str, object]) -> None:
    if not path.is_file() or path.stat().st_size < 1 or path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Pages archive is missing or outside the accepted size range")
    with path.open("rb") as stream:
        actual_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual_sha256 != expected_sha256:
        raise ValueError("Pages archive bytes do not match the selected digest")
    verify_archive_contents(path, selected)


def write_build_marker(path: Path) -> None:
    commit = os.environ["GITHUB_SHA"]
    if not SHA.fullmatch(commit):
        raise ValueError("docs build commit must be a full lowercase Git SHA")
    record = {
        "source_commit": commit,
        "source_run_id": positive_id(os.environ["GITHUB_RUN_ID"], "build run ID"),
        "source_attempt": positive_id(os.environ["GITHUB_RUN_ATTEMPT"], "build attempt"),
    }
    path.write_text(json.dumps(record, sort_keys=True) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("inputs", "verify", "marker"))
    parser.add_argument("--artifact-json", type=Path)
    parser.add_argument("--run-json", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.mode == "marker":
        if not args.output:
            parser.error("marker requires --output")
        write_build_marker(args.output)
        return
    selected = selection_from_environment()
    if args.mode == "inputs":
        print("Pages selection inputs are valid")
        return
    if not args.artifact_json or not args.run_json or not args.archive:
        parser.error("verify requires --artifact-json, --run-json, and --archive")
    artifact = json.loads(args.artifact_json.read_text())
    run = json.loads(args.run_json.read_text())
    verify_metadata(artifact, run, selected)
    verify_archive(args.archive, str(selected["archive_sha256"]), selected)
    print(f"Verified retained Pages artifact {selected['artifact_id']} from main {selected['source_commit']}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, tarfile.TarError) as error:
        raise SystemExit(f"Pages artifact validation failed: {error}") from None
