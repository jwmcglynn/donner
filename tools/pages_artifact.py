"""Validate a retained GitHub Pages artifact before manual publication."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile

from tools import release_cli


REPOSITORY = "jwmcglynn/donner"
WORKFLOW = ".github/workflows/deploy_docs.yaml"
BUILD_WORKFLOW = ".github/workflows/build_docs.yaml"
SOURCE_WORKFLOWS = (WORKFLOW, BUILD_WORKFLOW)
ARTIFACT_NAME = "github-pages"
SHA = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
MAX_ARCHIVE_BYTES = 1_000_000_000
MAX_ARCHIVE_FILES = 100_000
# The docs build retains this record and its generated lockfile beside the Pages artifact, in a
# separate `donner-docs-provenance-<attempt>` artifact, so the published site carries neither.
PROVENANCE_ARTIFACT_PREFIX = "donner-docs-provenance-"
PROVENANCE_FILES = {"provenance.json", release_cli.LOCKFILE}
PROVENANCE_SCHEMA = 1
PROVENANCE_KEYS = {
    "schema", "source_commit", "producer_run_id", "producer_attempt", "pages_archive_sha256",
    "generated_lock_sha256", "bazel_version", "doxygen_version", "graphviz_version",
}
MAX_PROVENANCE_BYTES = 64 * 1024
TOOL_VERSION = re.compile(r"[A-Za-z0-9 .+()~_:,/-]{1,256}\Z")
TOOL_TIMEOUT_SECONDS = 60


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
        run.get("path") not in SOURCE_WORKFLOWS
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


def fetch_metadata(selected: dict[str, object]) -> tuple[dict, dict]:
    """Read the selected artifact's and source run attempt's metadata from the Actions API."""
    repository = os.environ["GITHUB_REPOSITORY"]
    artifact = _github_json(f"repos/{repository}/actions/artifacts/{selected['artifact_id']}")
    run = _github_json(
        f"repos/{repository}/actions/runs/{selected['run_id']}/attempts/{selected['attempt']}")
    return artifact, run


def _github_json(endpoint: str) -> dict:
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))


def build_summary(archive: Path) -> str:
    """The job summary that records a retained docs build for a later manual deployment."""
    with archive.open("rb") as stream:
        archive_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    environ = os.environ
    attempt = environ.get("GITHUB_RUN_ATTEMPT", "")
    return "".join(line + "\n" for line in (
        "### Retained documentation artifact",
        f"- Source commit: `{environ.get('GITHUB_SHA', '')}`",
        f"- Build run: `{environ.get('GITHUB_RUN_ID', '')}`, attempt `{attempt}`",
        f"- Pages artifact ID: `{environ.get('ARTIFACT_ID', '')}`",
        f"- artifact.tar SHA-256: `{archive_sha256}`",
        "- Artifact metadata digest: retrieve from the Actions artifact API before manual deployment",
        f"- Build provenance: `{PROVENANCE_ARTIFACT_PREFIX}{attempt}`",
    ))


def write_build_marker(path: Path) -> None:
    record = _build_identity()
    path.write_text(json.dumps(record, sort_keys=True) + "\n", encoding="utf-8")


def tool_version(command: list[str]) -> str:
    """Return the first line a docs tool prints for its version, or fail closed."""
    try:
        result = subprocess.run(command, capture_output=True, check=False,
                                timeout=TOOL_TIMEOUT_SECONDS)
    except (OSError, subprocess.TimeoutExpired):
        raise ValueError("documentation tool version could not be read") from None
    try:
        text = (result.stdout or result.stderr).decode("ascii")
    except UnicodeDecodeError:
        raise ValueError("documentation tool version is not ASCII") from None
    version = text.splitlines()[0].strip() if text.strip() else ""
    if result.returncode != 0 or not TOOL_VERSION.fullmatch(version):
        raise ValueError("documentation tool version is missing or has unsupported characters")
    return version


def _build_identity() -> dict[str, object]:
    commit = os.environ["GITHUB_SHA"]
    if not SHA.fullmatch(commit):
        raise ValueError("docs build commit must be a full lowercase Git SHA")
    return {
        "source_commit": commit,
        "source_run_id": positive_id(os.environ["GITHUB_RUN_ID"], "build run ID"),
        "source_attempt": positive_id(os.environ["GITHUB_RUN_ATTEMPT"], "build attempt"),
    }


def write_provenance(archive: Path, lockfile: Path, output: Path, root: Path,
                     doxygen_version: str, graphviz_version: str) -> dict[str, object]:
    """Record the docs build's dependency state: its resolved module graph and its tools.

    The site is generated by Doxygen and Graphviz, not by Bazel, so their versions are the tools
    that produced its bytes. The lockfile is the production module graph of the documented
    `donner` module, resolved at build time; the docs site resolves no development modules.
    """
    identity = _build_identity()
    if not archive.is_file() or archive.is_symlink():
        raise ValueError("Pages archive is missing or linked")
    for version in (doxygen_version, graphviz_version):
        if not TOOL_VERSION.fullmatch(version):
            raise ValueError("documentation tool version is missing or has unsupported characters")
    lock_sha256 = release_cli.check_public_lockfile(lockfile)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("docs provenance output directory must be empty")
    shutil.copyfile(lockfile, output / release_cli.LOCKFILE)
    if release_cli.check_public_lockfile(output / release_cli.LOCKFILE) != lock_sha256:
        raise ValueError("retained docs lockfile changed while it was copied")
    with archive.open("rb") as stream:
        archive_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    provenance = {
        "schema": PROVENANCE_SCHEMA,
        "source_commit": identity["source_commit"],
        "producer_run_id": identity["source_run_id"],
        "producer_attempt": identity["source_attempt"],
        "pages_archive_sha256": archive_sha256,
        "generated_lock_sha256": lock_sha256,
        "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
        "doxygen_version": doxygen_version,
        "graphviz_version": graphviz_version,
    }
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
    return provenance


def verify_provenance(directory: Path, selected: dict[str, object], bazel_version: str) -> str:
    """Check retained docs provenance against the selected Pages build; return its lock digest."""
    if {path.name for path in directory.iterdir()} != PROVENANCE_FILES:
        raise ValueError("docs provenance artifact has missing or unexpected files")
    path = directory / "provenance.json"
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_PROVENANCE_BYTES:
        raise ValueError("docs provenance is missing, linked or too large")
    try:
        provenance = json.loads(path.read_bytes())
    except (ValueError, UnicodeDecodeError):
        raise ValueError("docs provenance is malformed") from None
    lock_sha256 = release_cli.check_public_lockfile(directory / release_cli.LOCKFILE)
    if (not isinstance(provenance, dict) or set(provenance) != PROVENANCE_KEYS or
            provenance["schema"] != PROVENANCE_SCHEMA or
            provenance["source_commit"] != selected["source_commit"] or
            type(provenance["producer_run_id"]) is not int or
            provenance["producer_run_id"] != selected["run_id"] or
            type(provenance["producer_attempt"]) is not int or
            provenance["producer_attempt"] != selected["attempt"] or
            provenance["pages_archive_sha256"] != selected["archive_sha256"] or
            provenance["generated_lock_sha256"] != lock_sha256 or
            provenance["bazel_version"] != bazel_version or
            any(not isinstance(provenance[key], str) or not TOOL_VERSION.fullmatch(provenance[key])
                for key in ("doxygen_version", "graphviz_version"))):
        raise ValueError("docs provenance does not match the selected Pages build")
    return lock_sha256


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("inputs", "verify", "marker", "provenance", "summary"))
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--lockfile", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.mode == "provenance":
        if not args.archive or not args.lockfile or not args.output:
            parser.error("provenance requires --archive, --lockfile, and --output")
        write_provenance(args.archive, args.lockfile, args.output, Path.cwd(),
                         tool_version(["doxygen", "--version"]), tool_version(["dot", "-V"]))
        print(f"Recorded docs build provenance in {args.output}")
        return
    if args.mode == "summary":
        if not args.archive:
            parser.error("summary requires --archive")
        summary = build_summary(args.archive)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
                output.write(summary)
        else:
            print(summary, end="")
        return
    if args.mode == "marker":
        if not args.output:
            parser.error("marker requires --output")
        write_build_marker(args.output)
        return
    selected = selection_from_environment()
    if args.mode == "inputs":
        print("Pages selection inputs are valid")
        return
    if not args.archive:
        parser.error("verify requires --archive")
    artifact, run = fetch_metadata(selected)
    verify_metadata(artifact, run, selected)
    verify_archive(args.archive, str(selected["archive_sha256"]), selected)
    print(f"Verified retained Pages artifact {selected['artifact_id']} from main {selected['source_commit']}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Pages artifact validation failed: {error}") from None
