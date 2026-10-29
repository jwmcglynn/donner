#!/usr/bin/env python3
"""Verify an immutable Donner release-candidate record against retained bytes and GitHub."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import zipfile
import zlib

from tools import bcr_source, pages_artifact, release_cli, release_macos_editor


REPOSITORY = "jwmcglynn/donner"
# Schema 2 added the `docs-provenance` role and the editor and docs generated lockfile digests.
# Schema 3 added the admission-validated registry entry; schema 4 requires the native editor ZIP.
SCHEMA = 4
ARTIFACT_WORKFLOWS = {
    "source": ".github/workflows/bcr_preflight.yml",
    "cli-linux": ".github/workflows/bcr_preflight.yml",
    "cli-macos": ".github/workflows/bcr_preflight.yml",
    "editor-macos": ".github/workflows/bcr_preflight.yml",
    # The entry preflight generated and ran through the upstream BCR validator. BCR fork
    # preparation submits exactly these bytes; it never regenerates the entry.
    "bcr-entry": ".github/workflows/bcr_preflight.yml",
    "editor": ".github/workflows/editor_wasm.yml",
    "docs": ".github/workflows/deploy_docs.yaml",
    # Retained beside the Pages artifact by the same docs build run; never published.
    "docs-provenance": ".github/workflows/deploy_docs.yaml",
}
# Release publishes exactly these roles' files. Docs deployment and BCR preparation are separate
# maintainer decisions with their own workflows.
RELEASE_ROLES = ("source", "cli-linux", "cli-macos", "editor-macos")
# One BCR Preflight attempt produces all of these.
PREFLIGHT_ROLES = (*RELEASE_ROLES, "bcr-entry")
# Each published artifact's build records its generated MODULE.bazel.lock digest in provenance.
LOCK_ROLES = ("cli-linux", "cli-macos", "editor-macos", "editor", "docs")
# The release stages each CLI role in a directory of its name; `release_cli` verifies them there.
CLI_PLATFORMS = release_cli.STAGED_PLATFORMS
COMPILER_ROLES = (*CLI_PLATFORMS, "editor-macos")
# The manual workflow that assembles a record, and the artifact that retains its files.
ASSEMBLY_WORKFLOW = ".github/workflows/release_candidate.yml"
RECORD_ARTIFACT_PREFIX = "donner-release-candidate-"
RECORD_NAME = "candidate.json"
EVIDENCE_WORKFLOWS = {
    "ci": ".github/workflows/main.yml",
    "coverage": ".github/workflows/coverage.yml",
    "fuzz": ".github/workflows/fuzz.yml",
    "sanitizers": ".github/workflows/sanitizers.yml",
}
REQUIRED_EVIDENCE_JOBS = {
    "ci": (("linux", "linux-self-hosted"), ("macos", "macos-self-hosted")),
    "coverage": (("build",), ("upload-coverage",)),
    "fuzz": (("linux",), ("macos",)),
    "sanitizers": (("asan",), ("ubsan",)),
}
REVIEW_NAMES = {"security", "supply-chain"}
RECORD_FILES = frozenset({
    RECORD_NAME, f"{RECORD_NAME}.sha256",
    *(f"evidence/{kind}.json" for kind in EVIDENCE_WORKFLOWS),
    *(f"reviews/{name}.md" for name in REVIEW_NAMES),
})
# Dispatch inputs that name the qualifying attempts: one per artifact-producing workflow, then one
# per evidence role.
ARTIFACT_RUN_INPUTS = {
    "preflight": PREFLIGHT_ROLES,
    "editor": ("editor",),
    # Docs provenance first: it names the Pages archive that the requested attempt built.
    "docs": ("docs-provenance", "docs"),
}
RUN_REFERENCE = re.compile(r"([1-9][0-9]{0,15})/([1-9][0-9]{0,4})\Z")
DECIMAL_ID = re.compile(r"[1-9][0-9]{0,15}\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
STABLE_VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
# Every retained artifact must stay downloadable this long after verification: up to 30 days from
# verification to publication, then a 30-day post-release rollback window in which the candidate's
# own bytes can be promoted again. docs/release_candidate.md documents this window.
MIN_REMAINING_RETENTION = timedelta(days=60)
# Release re-verifies the record at publication and still needs the post-release rollback window.
# A record assembled with 60 days left can therefore be published for up to 30 days.
PUBLICATION_REMAINING_RETENTION = timedelta(days=30)
MAX_ZIP_BYTES = 1_000_000_000
MAX_UNPACKED_BYTES = 2_000_000_000
MAX_FILES = 100_000
MAX_RECORD_BYTES = 10_000_000
MAX_SMALL_CONTROL_BYTES = 1_000_000
MAX_LOCK_BYTES = 16_000_000
MAX_REVIEW_BYTES = 10_000_000
EVIDENCE_KEYS = {"workflow", "run_id", "attempt", "commit", "jobs"}
EDITOR_COMMON_FILES = {
    "CatalogFontNotices.txt", "catalog-fonts.js", "catalog-fonts.json", "donner_icon.svg",
    "editor-bootstrap.js", "editor.css", "editor.js", "enable-threads.js", "index.html",
}


# Retained candidates from older source revisions legitimately lack this script.
EDITOR_RESOURCE_FILES = {f"{prefix}/{name}" for prefix in ("site", "deploy")
                         for name in ("packaged-resources.js", "packaged-resources.json",
                                      "packaged-resource-loader.js")}

EDITOR_DIALOG_FILES = {"site/browser-file-dialogs.js", "deploy/browser-file-dialogs.js"}


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
    if (not isinstance(record, dict) or
            set(record) != {"schema", "source", "artifacts", "evidence", "reviews"} or
            record["schema"] != SCHEMA):
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
    if not isinstance(source["version"], str) or not STABLE_VERSION.fullmatch(source["version"]):
        raise ValueError("candidate version must be a stable X.Y.Z release, not a prerelease")
    values = bcr_source.module_values((root / "MODULE.bazel").read_text(encoding="utf-8"))
    if (type(source["compatibility_level"]) is not int or
            source["version"] != values["version"] or
            source["compatibility_level"] != values.get("compatibility_level") or
            source["bazel_version"] != (root / ".bazelversion").read_text(encoding="utf-8").strip()):
        raise ValueError("candidate module or toolchain metadata differs from source")
    locks = source["module_locks_sha256"]
    compilers = source["compiler_versions"]
    if (not isinstance(locks, dict) or not isinstance(compilers, dict) or
            set(locks) != set(LOCK_ROLES) or set(compilers) != set(COMPILER_ROLES)):
        raise ValueError("candidate needs per-artifact lock and per-platform compiler records")
    for role in LOCK_ROLES:
        _digest(locks[role], "module lock digest")
    for role in COMPILER_ROLES:
        if not isinstance(compilers[role], str) or not re.fullmatch(r"[A-Za-z0-9 .+()~_-]{1,256}", compilers[role]):
            raise ValueError("candidate compiler version is invalid")
    return commit, tree


def _github_json(*args: str) -> dict:
    return json.loads(subprocess.check_output(["gh", "api", *args], text=True))


def _verify_run(run: dict, run_id: int, attempt: int, commit: str, workflow: str) -> None:
    paths = pages_artifact.SOURCE_WORKFLOWS if workflow == ARTIFACT_WORKFLOWS["docs"] else (workflow,)
    if (run.get("id") != run_id or run.get("run_attempt") != attempt or
            run.get("repository", {}).get("full_name") != REPOSITORY or
            run.get("path") not in paths or run.get("head_sha") != commit or
            run.get("head_branch") != "main" or
            run.get("event") not in ("push", "workflow_dispatch", "schedule") or
            run.get("status") != "completed" or run.get("conclusion") != "success"):
        raise ValueError("candidate evidence run is not a successful exact-source attempt")
    if workflow == ARTIFACT_WORKFLOWS["docs"] and (run.get("event") != "push" or run.get("head_branch") != "main"):
        raise ValueError("documentation artifact must come from a main push")


def _expiry(artifact: dict) -> datetime | None:
    value = artifact.get("expires_at")
    if not isinstance(value, str):
        return None
    try:
        expires = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return expires if expires.tzinfo is not None else None


def _verify_retention(artifact: dict, role: str, now: datetime,
                      minimum: timedelta = MIN_REMAINING_RETENTION) -> None:
    expires = _expiry(artifact)
    if expires is None or expires - now < minimum:
        raise ValueError(f"{role} artifact expires before the release and rollback window ends")


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


# Roles whose producers name the artifact without the run attempt, so a rerun can leave several.
ATTEMPTLESS_ROLES = ("editor", "docs")


def _artifact_name(role: str, commit: str, attempt: int) -> str:
    return {
        "source": f"donner-bcr-qualified-{attempt}",
        "cli-linux": f"donner-cli-linux-x86-64-{commit}-{attempt}",
        "cli-macos": f"donner-cli-darwin-arm64-{commit}-{attempt}",
        "editor-macos": f"donner-editor-darwin-arm64-{commit}-{attempt}",
        "bcr-entry": f"donner-bcr-entry-{attempt}",
        "editor": f"donner-editor-wasm-geode-{commit}",
        "docs": pages_artifact.ARTIFACT_NAME,
        "docs-provenance": f"{pages_artifact.PROVENANCE_ARTIFACT_PREFIX}{attempt}",
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
    if role == "editor-macos":
        return set(release_macos_editor.NAMES)
    if role == "docs":
        return {"artifact.tar"}
    if role == "docs-provenance":
        return set(pages_artifact.PROVENANCE_FILES)
    if role == "bcr-entry":
        return {bcr_source.ADMISSION_REPORT, "registry/bazel_registry.json",
                *(f"registry/{path}" for path in bcr_source.entry_paths(version))}
    return None


def _previous_registry_versions(names: set[str], version: str) -> set[str]:
    """Files of earlier donner versions that preflight copied from upstream into its registry."""
    previous = set()
    for name in names:
        parts = PurePosixPath(name).parts
        if (len(parts) >= 5 and parts[:3] == ("registry", "modules", "donner") and
                parts[3] != version and bcr_source.VERSION.fullmatch(parts[3])):
            previous.add(name)
    return previous


def _editor_font_files(names: set[str]) -> set[str]:
    fonts = {name.removeprefix("site/") for name in names if name.startswith("site/fonts/")}
    if (any(not re.fullmatch(r"fonts/[0-9a-f]{64}\.woff2", font) for font in fonts) or
            {f"deploy/{font}" for font in fonts} !=
            {name for name in names if name.startswith("deploy/fonts/")}):
        raise ValueError("editor font paths are outside the published catalog shape")
    return {f"{prefix}/{font}" for prefix in ("site", "deploy") for font in fonts}


def _editor_resource_files(names: set[str]) -> set[str]:
    assets = {name.removeprefix("site/") for name in names if name.startswith("site/assets/")}
    deployed = {name for name in names if name.startswith("deploy/assets/")}
    if (any(not re.fullmatch(r"assets/[0-9a-f]{64}\.(?:ttf|otf|woff2|svg)", name)
            for name in assets) or {f"deploy/{name}" for name in assets} != deployed):
        raise ValueError("editor resource paths are outside the published asset shape")
    if (assets or names & EDITOR_RESOURCE_FILES) and not EDITOR_RESOURCE_FILES <= names:
        raise ValueError("editor resource metadata is incomplete")
    return {f"{prefix}/{name}" for prefix in ("site", "deploy") for name in assets}


def _verify_role_files(role: str, names: set[str], version: str) -> None:
    required = _fixed_role_files(role, version)
    if required is None:
        required = {"provenance.json", "SHA256SUMS", release_cli.LOCKFILE, "site/editor.wasm",
                    "deploy/editor.wasm.gz"}
        required.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                        for name in EDITOR_COMMON_FILES)
        required.update(_editor_font_files(names))
        required.update(_editor_resource_files(names))
        if names & EDITOR_RESOURCE_FILES:
            required.update(EDITOR_RESOURCE_FILES)
        if names & EDITOR_DIALOG_FILES:
            required.update(EDITOR_DIALOG_FILES)
    elif role == "bcr-entry":
        required |= _previous_registry_versions(names - required, version)
    if names != required:
        raise ValueError("candidate artifact inventory differs from its publishable role")


def _member_size_limit(name: str) -> int:
    if name in ("SHA256SUMS", "provenance.json") or name.endswith(
        (".provenance", ".provenance.json", ".sha256")
    ):
        return MAX_SMALL_CONTROL_BYTES
    if name.endswith(".bazel.lock"):
        return MAX_LOCK_BYTES
    if name.endswith(".NOTICE.txt"):
        return release_cli.MAX_NOTICE_BYTES
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


def _checked_member(item: zipfile.ZipInfo, total: int) -> int:
    """Reject a link or an oversized member; return the running unpacked byte count."""
    mode = item.external_attr >> 16
    if (stat.S_IFMT(mode) not in (0, stat.S_IFREG) or
            item.file_size > MAX_UNPACKED_BYTES - total or
            item.file_size > _member_size_limit(item.filename)):
        raise ValueError("candidate artifact ZIP has a link or exceeds the byte budget")
    return total + item.file_size


def _zip_inventory(zip_path: Path) -> dict[str, str]:
    """Map every regular member of a downloaded artifact ZIP to its uncompressed SHA-256."""
    if not zip_path.is_file() or zip_path.is_symlink() or zip_path.stat().st_size > MAX_ZIP_BYTES:
        raise ValueError("candidate artifact ZIP is missing, linked, or too large")
    files: dict[str, str] = {}
    total = 0
    with zipfile.ZipFile(zip_path) as archive:
        for item in _bounded_members(archive):
            _safe_zip_path(item.filename)
            if item.is_dir():
                continue
            if item.filename in files:
                raise ValueError("candidate artifact ZIP has duplicate or unexpected files")
            total = _checked_member(item, total)
            checksum = hashlib.sha256()
            with archive.open(item) as source:
                while chunk := source.read(1024 * 1024):
                    checksum.update(chunk)
            files[item.filename] = checksum.hexdigest()
    if not files:
        raise ValueError("candidate artifact ZIP has no files")
    return files


def _zip_json(zip_path: Path, name: str, label: str) -> dict:
    with zipfile.ZipFile(zip_path) as archive:
        try:
            item = archive.getinfo(name)
        except KeyError:
            raise ValueError(f"{label} is missing") from None
        if item.file_size > MAX_SMALL_CONTROL_BYTES:
            raise ValueError(f"{label} is too large")
        data = archive.read(item)
    try:
        value = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError(f"{label} is not JSON") from None
    if not isinstance(value, dict):
        raise ValueError(f"{label} is not a JSON object")
    return value


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
            total = _checked_member(item, total)
            seen.add(name)
            _copy_checked_entry(archive, item, expected[name], output)
    if seen != set(expected):
        raise ValueError("candidate artifact ZIP is missing retained files")


def _inflate_editor_chunk(decoder, block: bytes, remaining: int) -> bytes:
    try:
        data = decoder.decompress(block, min(remaining + 1, 1024 * 1024))
    except zlib.error:
        raise ValueError("editor deployed gzip is corrupt") from None
    if len(data) > remaining:
        raise ValueError("editor deployed gzip exceeds the retained raw payload size")
    return data


def _require_editor_gzip_eof(decoder, stream) -> None:
    if decoder.unused_data or decoder.unconsumed_tail or stream.read(1):
        raise ValueError("editor deployed gzip contains trailing data")


def _deployed_editor_chunks(stream, maximum: int):
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    remaining = maximum
    for block in iter(lambda: stream.read(64 * 1024), b""):
        while block:
            data = _inflate_editor_chunk(decoder, block, remaining)
            remaining -= len(data)
            yield data
            if decoder.eof:
                _require_editor_gzip_eof(decoder, stream)
                return
            tail = decoder.unconsumed_tail
            if not data and tail == block:
                raise ValueError("editor deployed gzip makes no progress")
            block = tail
    raise ValueError("editor deployed gzip is truncated")


def _verify_editor_wasm(raw: Path, served: Path, identity: bytes) -> None:
    size = raw.stat().st_size
    if size > MAX_UNPACKED_BYTES or served.stat().st_size > MAX_UNPACKED_BYTES:
        raise ValueError("editor deployed or raw payload exceeds the artifact size limit")
    tail, found = b"", False
    with raw.open("rb") as original, served.open("rb") as compressed:
        for data in _deployed_editor_chunks(compressed, size):
            if original.read(len(data)) != data:
                raise ValueError("editor deployed payload differs from the retained raw payload")
            window = tail + data
            found = found or identity in window
            tail = window[-len(identity):]
        if original.read(1):
            raise ValueError("editor deployed payload is shorter than the retained raw payload")
    if not found:
        raise ValueError("editor package does not contain the selected build identity")


def _verify_editor_dialog_script(root: Path, expected: dict[str, str]) -> None:
    # The caller binds root to the candidate's clean exact-source checkout, including for rollback.
    script = root / "donner/editor/wasm/browser-file-dialogs.js"
    source_digest = _sha256(script) if script.exists() or script.is_symlink() else None
    required = {} if source_digest is None else dict.fromkeys(EDITOR_DIALOG_FILES, source_digest)
    actual = {name: expected[name] for name in EDITOR_DIALOG_FILES if name in expected}
    if actual != required:
        raise ValueError("editor dialog script differs from the selected source")


def _verify_editor(directory: Path, commit: str, root: Path, expected: dict[str, str],
                   run_id: str, attempt: str) -> str:
    """Check the editor package; return the digest of the module lock its build generated.

    `lockfile_sha256` is the npm lock of the browser test harness; `generated_lock_sha256` is the
    `MODULE.bazel.lock` the package build resolved and proved with `--lockfile_mode=error`.
    """
    provenance = json.loads((directory / "provenance.json").read_text(encoding="utf-8"))
    generated_lock = release_cli.check_public_lockfile(directory / release_cli.LOCKFILE)
    required_keys = {"source_revision", "producer_run_id", "producer_attempt",
                     "lockfile_sha256", "generated_lock_sha256", "bazel_configs", "targets",
                     "renderer", "transport_encoding"}
    if (not isinstance(provenance, dict) or set(provenance) != required_keys or
            provenance.get("source_revision") != commit or provenance.get("renderer") != "geode" or
            provenance.get("producer_run_id") != run_id or
            provenance.get("producer_attempt") != attempt or
            provenance.get("bazel_configs") != ["editor-wasm"] or
            provenance.get("transport_encoding") != "gzip" or
            not isinstance(provenance.get("targets"), list) or len(provenance["targets"]) != 1 or
            not isinstance(provenance["targets"][0], str) or
            not re.fullmatch(r"//[A-Za-z0-9_./-]+:[A-Za-z0-9_./-]+", provenance["targets"][0]) or
            provenance.get("lockfile_sha256") != _sha256(root / "donner/editor/wasm/tests/package-lock.json") or
            provenance.get("generated_lock_sha256") != generated_lock or
            expected.get(release_cli.LOCKFILE) != generated_lock):
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
    _verify_editor_dialog_script(root, expected)
    _verify_editor_wasm(directory / "site/editor.wasm", directory / "deploy/editor.wasm.gz",
                        release_macos_editor._expected_build_info(root, commit))
    return generated_lock


def _require_lock(role: str, actual: str, record: dict) -> None:
    lock_role = "docs" if role == "docs-provenance" else role
    if actual != record["source"]["module_locks_sha256"][lock_role]:
        raise ValueError(f"{lock_role} generated lock differs from candidate dependency state")


def _verify_source_payload(role, directory, entry, record, root, commit, run_id, attempt):
    receipt = bcr_source.verify_archive(directory, commit, run_id, attempt)
    if receipt["sha256"] != entry["files"][receipt["archive"]]:
        raise ValueError("source archive digest differs from candidate")


def _verify_cli_payload(role, directory, entry, record, root, commit, run_id, attempt):
    provenance = release_cli.verify(root=root, artifacts=directory, platform=CLI_PLATFORMS[role],
                                    commit=commit, run_id=run_id, attempt=attempt)
    if provenance["compiler_version"] != record["source"]["compiler_versions"][role]:
        raise ValueError("CLI compiler differs from candidate dependency state")
    _require_lock(role, provenance["generated_lock_sha256"], record)


def _verify_editor_macos_payload(role, directory, entry, record, root, commit, run_id, attempt):
    provenance = release_macos_editor.verify(root=root, artifacts=directory, commit=commit,
                                             run_id=run_id, attempt=attempt)
    if provenance["compiler_version"] != record["source"]["compiler_versions"][role]:
        raise ValueError("native editor compiler differs from candidate dependency state")
    _require_lock(role, provenance["generated_lock_sha256"], record)


def _verify_editor_payload(role, directory, entry, record, root, commit, run_id, attempt):
    _require_lock(role, _verify_editor(directory, commit, root, entry["files"], run_id, attempt),
                  record)


def _verify_docs_provenance_payload(role, directory, entry, record, root, commit, run_id, attempt):
    selected = {"run_id": entry["run_id"], "attempt": entry["attempt"], "source_commit": commit,
                "archive_sha256": record["artifacts"]["docs"]["files"]["artifact.tar"]}
    # The source check binds this to the checkout's `.bazelversion`; reading it from the record
    # lets docs deployment check provenance without a checkout of the candidate commit.
    bazel_version = record["source"]["bazel_version"]
    _require_lock(role, pages_artifact.verify_provenance(directory, selected, bazel_version), record)


def _verify_docs_payload(role, directory, entry, record, root, commit, run_id, attempt):
    selected = {"artifact_id": entry["artifact_id"], "run_id": entry["run_id"],
                "attempt": entry["attempt"], "source_commit": commit,
                "artifact_digest": "sha256:" + entry["zip_sha256"],
                "archive_sha256": entry["files"]["artifact.tar"]}
    pages_artifact.verify_archive(directory / "artifact.tar", selected["archive_sha256"], selected)


def _verify_bcr_entry_payload(role, directory, entry, record, root, commit, run_id, attempt):
    version = record["source"]["version"]
    archive = record["artifacts"]["source"]["files"][f"donner-{version}.tar.gz"]
    bcr_source.verify_validated_entry(directory, root, version, archive)


PAYLOAD_VERIFIERS = {
    "source": _verify_source_payload,
    "cli-linux": _verify_cli_payload,
    "cli-macos": _verify_cli_payload,
    "editor-macos": _verify_editor_macos_payload,
    "bcr-entry": _verify_bcr_entry_payload,
    "editor": _verify_editor_payload,
    "docs": _verify_docs_payload,
    "docs-provenance": _verify_docs_provenance_payload,
}


def _verify_payload(role: str, directory: Path, entry: dict, record: dict,
                    root: Path | None) -> None:
    """Run the role's own verifier on its extracted files and bind its dependency state."""
    PAYLOAD_VERIFIERS[role](role, directory, entry, record, root, record["source"]["commit"],
                            str(entry["run_id"]), str(entry["attempt"]))


def _check_jobs(jobs: object, kind: str, passed: int, skipped: int) -> None:
    if (not isinstance(jobs, list) or
            any(not isinstance(job, dict) or not isinstance(job.get("name"), str) or
                job.get("conclusion") not in ("success", "skipped") for job in jobs) or
            sum(job["conclusion"] == "success" for job in jobs) != passed or
            sum(job["conclusion"] == "skipped" for job in jobs) != skipped):
        raise ValueError("candidate workflow job counts are incomplete or not green")
    successful = {job["name"] for job in jobs if job["conclusion"] == "success"}
    if any(not successful.intersection(alternatives) for alternatives in REQUIRED_EVIDENCE_JOBS[kind]):
        raise ValueError("candidate qualification run skipped a required execution lane")


def _verify_job_counts(response: dict, kind: str, passed: int, skipped: int) -> None:
    jobs = response.get("jobs")
    if (not isinstance(jobs, list) or type(response.get("total_count")) is not int or
            len(jobs) != response["total_count"]):
        raise ValueError("candidate workflow job counts are incomplete or not green")
    _check_jobs(jobs, kind, passed, skipped)


def _job_outcomes(jobs: list) -> Counter:
    return Counter((job["name"], job["conclusion"]) for job in jobs)


def _read_bounded(path: Path, limit: int, label: str) -> bytes:
    if not path.is_file() or path.is_symlink() or path.stat().st_size > limit:
        raise ValueError(f"{label} is missing, linked, or too large")
    return path.read_bytes()


def _verify_evidence_content(path: Path, kind: str, entry: dict, commit: str,
                             api_jobs: list) -> None:
    """Require the evidence file to be the job record of exactly the bound attempt."""
    try:
        document = json.loads(_read_bounded(path, MAX_SMALL_CONTROL_BYTES, f"{kind} evidence"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError(f"{kind} evidence is not JSON") from None
    if (not isinstance(document, dict) or set(document) != EVIDENCE_KEYS or
            document["workflow"] != EVIDENCE_WORKFLOWS[kind] or
            document["run_id"] != entry["run_id"] or type(document["run_id"]) is not int or
            document["attempt"] != entry["attempt"] or type(document["attempt"]) is not int or
            document["commit"] != commit):
        raise ValueError(f"{kind} evidence does not name the candidate run, attempt and commit")
    _check_jobs(document["jobs"], kind, entry["passed_jobs"], entry["skipped_jobs"])
    if _job_outcomes(document["jobs"]) != _job_outcomes(api_jobs):
        raise ValueError(f"{kind} evidence jobs differ from the GitHub attempt")


def _check_artifact_entry(role: str, entry: dict, record: dict, github_json, now: datetime,
                          min_remaining: timedelta) -> tuple[int, int]:
    """Check one role's record entry against GitHub; return its run ID and attempt."""
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
    _verify_retention(metadata, role, now, min_remaining)
    if role == "docs":
        pages_artifact.verify_metadata(metadata, run, {
            "artifact_id": entry["artifact_id"], "run_id": run_id, "attempt": attempt,
            "source_commit": commit, "artifact_digest": "sha256:" + entry["zip_sha256"],
            "archive_sha256": entry["files"]["artifact.tar"],
        })
    return run_id, attempt


def _verify_one_artifact(role: str, entry: dict, record: dict, root: Path,
                         artifacts: Path, github_json, now: datetime,
                         min_remaining: timedelta) -> tuple[int, int]:
    identity = _check_artifact_entry(role, entry, record, github_json, now, min_remaining)
    zip_path = artifacts / f"{role}.zip"
    if not zip_path.is_file() or zip_path.is_symlink() or zip_path.stat().st_size > MAX_ZIP_BYTES:
        raise ValueError(f"{role} artifact ZIP is missing, linked, or too large")
    if _sha256(zip_path) != entry["zip_sha256"]:
        raise ValueError(f"{role} artifact ZIP digest differs from the candidate")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        _extract_verified(zip_path, entry["files"], directory)
        _verify_payload(role, directory, entry, record, root)
    return identity


def _verify_artifacts(record: dict, root: Path, artifacts: Path, github_json,
                      min_remaining: timedelta) -> None:
    now = datetime.now(timezone.utc)
    identities: dict[str, tuple[int, int]] = {}
    # The docs payload check reads the docs entry, so verify roles in their declared order.
    for role in ARTIFACT_WORKFLOWS:
        identities[role] = _verify_one_artifact(role, record["artifacts"][role], record, root,
                                                artifacts, github_json, now, min_remaining)
    if len({identities[role] for role in PREFLIGHT_ROLES}) != 1:
        raise ValueError("source, CLI and registry entry artifacts must share one qualified "
                         "preflight attempt")
    if identities["docs"] != identities["docs-provenance"]:
        raise ValueError("docs provenance must come from the docs build attempt")


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
        _verify_evidence_content(evidence / f"{kind}.json", kind, entry, commit, jobs["jobs"])


def _verify_review_content(path: Path, kind: str, commit: str) -> None:
    """Require a non-empty UTF-8 report that names the full candidate commit."""
    try:
        text = _read_bounded(path, MAX_REVIEW_BYTES, f"{kind} review report").decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError(f"{kind} review report is not UTF-8 text") from None
    if not text.strip() or "\x00" in text:
        raise ValueError(f"{kind} review report is empty or not text")
    if not re.search(rf"(?<![0-9A-Fa-f]){commit}(?![0-9A-Fa-f])", text):
        raise ValueError(f"{kind} review report does not name the candidate commit")


def _verify_reviews(record: dict, reviews: Path) -> None:
    commit = record["source"]["commit"]
    for kind, digest in record["reviews"].items():
        _digest(digest, "review report digest")
        if _sha256(reviews / f"{kind}.md") != digest:
            raise ValueError(f"{kind} review report digest differs from the candidate")
        _verify_review_content(reviews / f"{kind}.md", kind, commit)


def verify(record: dict, root: Path, artifacts: Path, evidence: Path, reviews: Path,
           github_json=_github_json, min_remaining: timedelta = MIN_REMAINING_RETENTION) -> None:
    _record_shape(record)
    _verify_source(record["source"], root)
    _verify_artifacts(record, root, artifacts, github_json, min_remaining)
    _verify_evidence(record, evidence, github_json)
    _verify_reviews(record, reviews)


def _download_artifact(artifact_id: int, path: Path) -> None:
    """Save the exact retained ZIP; GitHub's artifact digest is the SHA-256 of these bytes."""
    with path.open("xb") as stream:
        subprocess.run(["gh", "api", f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"],
                       stdout=stream, check=True)


def _empty_directory(path: Path, label: str) -> Path:
    if path.is_symlink() or (path.exists() and (not path.is_dir() or any(path.iterdir()))):
        raise ValueError(f"{label} directory must be new or empty")
    path.mkdir(parents=True, exist_ok=True)
    return path


def _artifact_digest(artifact: dict, label: str) -> str:
    value = artifact.get("digest")
    if not isinstance(value, str) or not value.startswith("sha256:"):
        raise ValueError(f"{label} has no GitHub SHA-256 digest")
    return _digest(value.removeprefix("sha256:"), f"{label} digest")


def _download_checked(artifact_id: int, digest: str, path: Path, download, label: str) -> None:
    download(artifact_id, path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ZIP_BYTES:
        raise ValueError(f"{label} download is missing, linked, or too large")
    if _sha256(path) != digest:
        raise ValueError(f"{label} download differs from its GitHub digest")


def _built_by_attempt(role: str, path: Path, run_id: int, attempt: int) -> bool:
    """Whether a downloaded editor or docs artifact names the requested attempt as its producer.

    Their names omit the attempt, so a rerun keeps an earlier attempt's artifact under the same
    name. The editor package records its producer run and attempt; the Pages archive is the one
    that attempt's docs provenance (collected first, into the same directory) names.
    """
    if role == "editor":
        provenance = _zip_json(path, "provenance.json", "editor provenance")
        return (provenance.get("producer_run_id") == str(run_id) and
                provenance.get("producer_attempt") == str(attempt))
    provenance = _zip_json(path.with_name("docs-provenance.zip"), "provenance.json",
                           "docs provenance")
    return _zip_inventory(path).get("artifact.tar") == provenance.get("pages_archive_sha256")


def _named_artifacts(role: str, name: str, run_id: int, github_json) -> list[dict]:
    """The run's retained artifacts named `name`: one, or for an attemptless role one or more."""
    listing = github_json(
        f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100&name={name}")
    found = listing.get("artifacts") if isinstance(listing, dict) else None
    matches = [item for item in found if isinstance(item, dict) and item.get("name") == name] \
        if isinstance(found, list) else []
    if not matches or any(item.get("expired") is not False for item in matches) or \
            (len(matches) > 1 and role not in ATTEMPTLESS_ROLES):
        raise ValueError(f"{role} needs exactly one retained artifact named {name} in its run")
    return matches


def _collect_artifact(role: str, commit: str, run_id: int, attempt: int, path: Path,
                      github_json, download) -> dict:
    name = _artifact_name(role, commit, attempt)
    matches = _named_artifacts(role, name, run_id, github_json)
    selected = []
    for index, item in enumerate(matches):
        artifact_id = _positive(item.get("id"), f"{role} artifact ID")
        zip_sha256 = _artifact_digest(item, f"{role} artifact")
        candidate = path if len(matches) == 1 else path.with_name(f"{role}.{index}.zip")
        _download_checked(artifact_id, zip_sha256, candidate, download, f"{role} artifact")
        if len(matches) == 1 or _built_by_attempt(role, candidate, run_id, attempt):
            selected.append((artifact_id, zip_sha256, candidate))
        else:
            candidate.unlink()
    if len(selected) != 1:
        raise ValueError(f"{role} needs exactly one retained artifact named {name} built by "
                         f"attempt {attempt} of its run")
    artifact_id, zip_sha256, chosen = selected[0]
    chosen.rename(path)
    return {"run_id": run_id, "attempt": attempt, "artifact_id": artifact_id, "name": name,
            "zip_sha256": zip_sha256, "files": _zip_inventory(path)}


def _assembled_source(commit: str, root: Path, zips: Path) -> dict:
    """Read the source identity from the checkout and the dependency state from provenance."""
    values = bcr_source.module_values((root / "MODULE.bazel").read_text(encoding="utf-8"))
    locks: dict[str, object] = {}
    compilers: dict[str, object] = {}
    for role, platform in CLI_PLATFORMS.items():
        provenance = _zip_json(zips / f"{role}.zip", release_cli._names(platform)[2],
                               f"{role} provenance")
        locks[role] = provenance.get("generated_lock_sha256")
        compilers[role] = provenance.get("compiler_version")
    native = _zip_json(zips / "editor-macos.zip", release_macos_editor.NAMES[2],
                       "native editor provenance")
    locks["editor-macos"] = native.get("generated_lock_sha256")
    compilers["editor-macos"] = native.get("compiler_version")
    locks["editor"] = _zip_json(zips / "editor.zip", "provenance.json",
                                "editor provenance").get("generated_lock_sha256")
    locks["docs"] = _zip_json(zips / "docs-provenance.zip", "provenance.json",
                              "docs provenance").get("generated_lock_sha256")
    return {"commit": commit, "tree": _git(root, "rev-parse", "HEAD^{tree}"),
            "version": values["version"],
            "compatibility_level": values.get("compatibility_level"),
            "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
            "module_locks_sha256": locks, "compiler_versions": compilers}


def _collect_evidence(kind: str, commit: str, run_id: int, attempt: int, path: Path,
                      github_json) -> dict:
    """Snapshot the attempt's jobs as the retained evidence file the record binds."""
    response = github_json(
        f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100")
    jobs = response.get("jobs") if isinstance(response, dict) else None
    if not isinstance(jobs, list) or any(not isinstance(job, dict) for job in jobs):
        raise ValueError(f"{kind} evidence jobs are unavailable")
    summary = [{"name": job.get("name"), "conclusion": job.get("conclusion")} for job in jobs]
    document = {"workflow": EVIDENCE_WORKFLOWS[kind], "run_id": run_id, "attempt": attempt,
                "commit": commit, "jobs": summary}
    data = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode()
    path.write_bytes(data)
    return {"run_id": run_id, "attempt": attempt,
            "passed_jobs": sum(job["conclusion"] == "success" for job in summary),
            "skipped_jobs": sum(job["conclusion"] == "skipped" for job in summary),
            "sha256": hashlib.sha256(data).hexdigest()}


def _collect_review(kind: str, comment_id: int, path: Path, github_json) -> str:
    """Retain a review report posted by the repository owner as a comment on one of its issues."""
    comment = github_json(f"repos/{REPOSITORY}/issues/comments/{comment_id}")
    issue = comment.get("issue_url") if isinstance(comment, dict) else None
    if (not isinstance(comment, dict) or comment.get("id") != comment_id or
            not isinstance(issue, str) or
            not re.fullmatch(rf"https://api\.github\.com/repos/{REPOSITORY}/issues/[1-9][0-9]*",
                             issue) or
            comment.get("author_association") != "OWNER" or
            not isinstance(comment.get("body"), str)):
        raise ValueError(f"{kind} review must be the repository owner's comment on an issue "
                         f"in {REPOSITORY}")
    data = comment["body"].encode("utf-8")
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def assemble(*, commit: str, runs: dict[str, tuple[int, int]], reviews: dict[str, int],
             root: Path, work: Path, output: Path, github_json=_github_json,
             download=_download_artifact) -> bytes:
    """Build the record from named attempts, verify it, and write it with its evidence files.

    Nothing is selected implicitly: every run and attempt and both review comments are inputs.
    The record is written only after it verifies, with the full 60-day retention window.
    """
    _commit(commit, "candidate commit")
    if _git(root, "rev-parse", "HEAD") != commit:
        raise ValueError("assembly must run on the candidate commit: dispatch it on main while "
                         "main points at that commit")
    if (set(runs) != set(ARTIFACT_RUN_INPUTS) | set(EVIDENCE_WORKFLOWS) or
            set(reviews) != REVIEW_NAMES):
        raise ValueError("assembly needs every qualifying run and both review reports")
    zips = _empty_directory(work, "assembly work") / "artifacts"
    zips.mkdir()
    _empty_directory(output, "assembly output")
    artifacts = {}
    for run_input, roles in ARTIFACT_RUN_INPUTS.items():
        run_id, attempt = runs[run_input]
        for role in roles:
            artifacts[role] = _collect_artifact(role, commit, run_id, attempt,
                                                zips / f"{role}.zip", github_json, download)
    evidence_dir = output / "evidence"
    evidence_dir.mkdir()
    evidence = {kind: _collect_evidence(kind, commit, *runs[kind], evidence_dir / f"{kind}.json",
                                        github_json)
                for kind in EVIDENCE_WORKFLOWS}
    review_dir = output / "reviews"
    review_dir.mkdir()
    review_digests = {kind: _collect_review(kind, reviews[kind], review_dir / f"{kind}.md",
                                            github_json)
                      for kind in sorted(REVIEW_NAMES)}
    record = {"schema": SCHEMA, "source": _assembled_source(commit, root, zips),
              "artifacts": artifacts, "evidence": evidence, "reviews": review_digests}
    raw = _canonical_bytes(record)
    verify(json.loads(raw), root, zips, evidence_dir, review_dir, github_json)
    (output / RECORD_NAME).write_bytes(raw)
    (output / f"{RECORD_NAME}.sha256").write_text(
        f"{hashlib.sha256(raw).hexdigest()}  {RECORD_NAME}\n", encoding="ascii")
    return raw


def _load_canonical(raw: bytes) -> dict:
    try:
        record = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("candidate record is not JSON") from None
    if not isinstance(record, dict) or raw != _canonical_bytes(record):
        raise ValueError("candidate record is not canonical immutable JSON")
    return record


def load_record(raw: bytes, record_sha256: str) -> dict:
    """Parse record bytes that must have exactly the digest that names them."""
    _digest(record_sha256, "candidate record digest")
    if len(raw) > MAX_RECORD_BYTES or hashlib.sha256(raw).hexdigest() != record_sha256:
        raise ValueError("candidate record digest differs from the approved record")
    record = _load_canonical(raw)
    _record_shape(record)
    return record


def _verify_record_artifact(metadata: dict, artifact_id: int, run_id: int, attempt: int,
                            commit: str, now: datetime,
                            minimum: timedelta = PUBLICATION_REMAINING_RETENTION) -> str:
    run = metadata.get("workflow_run") or {}
    size = metadata.get("size_in_bytes")
    if (metadata.get("id") != artifact_id or
            metadata.get("name") != f"{RECORD_ARTIFACT_PREFIX}{attempt}" or
            metadata.get("expired") is not False or
            type(size) is not int or not 0 < size <= MAX_ZIP_BYTES or
            run.get("id") != run_id or run.get("head_sha") != commit or
            run.get("head_branch") != "main" or
            run.get("repository_id") is None or
            run.get("head_repository_id") != run.get("repository_id")):
        raise ValueError("candidate record artifact does not belong to the named assembly attempt")
    _verify_retention(metadata, "candidate record", now, minimum)
    return _artifact_digest(metadata, "candidate record artifact")


def _fetch_record(artifact_id: int, run_id: int, attempt: int, commit: str, record_sha256: str,
                  named_by: str, work: Path, github_json, download,
                  minimum: timedelta = PUBLICATION_REMAINING_RETENTION) -> tuple[bytes, dict, Path]:
    """Download the record artifact of one assembly attempt; return its bytes, record and files."""
    metadata = github_json(f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}")
    zip_sha256 = _verify_record_artifact(metadata, artifact_id, run_id, attempt, commit,
                                         datetime.now(timezone.utc), minimum)
    record_zip = work / "record.zip"
    _download_checked(artifact_id, zip_sha256, record_zip, download, "candidate record artifact")
    files = _zip_inventory(record_zip)
    if set(files) != RECORD_FILES:
        raise ValueError("candidate record artifact has missing or unexpected files")
    record_dir = work / "record"
    _extract_verified(record_zip, files, record_dir)
    raw = _read_bounded(record_dir / RECORD_NAME, MAX_RECORD_BYTES, "candidate record")
    if hashlib.sha256(raw).hexdigest() != record_sha256:
        raise ValueError(f"candidate record digest differs from the {named_by}")
    if (record_dir / f"{RECORD_NAME}.sha256").read_bytes() != \
            f"{record_sha256}  {RECORD_NAME}\n".encode():
        raise ValueError("candidate record checksum file differs from the record")
    record = _load_canonical(raw)
    _record_shape(record)
    return raw, record, record_dir


# The `root` of a role whose checks read no source files, such as docs provenance, which is
# checked against the record's Bazel version.
NO_SOURCE_CHECKOUT = None


def fetch_role(record: dict, role: str, root: Path | None, zip_path: Path, output: Path,
               github_json=_github_json, download=_download_artifact) -> Path:
    """Download one role's artifact by its recorded ID, verify it and extract it to `output`.

    This runs the same checks as the verifier for that role. It is used after a record was
    published, so the artifact only has to be retained still, not for the assembly window.
    """
    if role == "editor":
        if root is None:
            raise ValueError("editor artifact needs a clean exact-source checkout")
        _verify_source(record["source"], root)
    entry = record["artifacts"][role]
    _check_artifact_entry(role, entry, record, github_json, datetime.now(timezone.utc),
                          timedelta(0))
    _download_checked(entry["artifact_id"], entry["zip_sha256"], zip_path, download,
                      f"{role} artifact")
    _empty_directory(output, f"{role} output")
    _extract_verified(zip_path, entry["files"], output)
    _verify_payload(role, output, entry, record, root)
    return output


def release(*, root: Path, record_run_id: int, record_attempt: int, record_artifact_id: int,
            record_sha256: str, work: Path, output: Path, github_json=_github_json,
            download=_download_artifact) -> dict[str, str]:
    """Re-verify the named record and stage exactly the release roles' retained files.

    Release names the assembly attempt and the record digest in its body. This downloads that
    record and every artifact it binds by artifact ID, verifies them again with the publication
    retention window, and extracts only the source and CLI roles' files for upload.
    """
    _digest(record_sha256, "candidate record digest")
    commit = _git(root, "rev-parse", "HEAD")
    _empty_directory(work, "release work")
    _empty_directory(output, "release output")
    raw, record, record_dir = _fetch_record(record_artifact_id, record_run_id, record_attempt,
                                            commit, record_sha256, "release body", work,
                                            github_json, download)
    zips = work / "artifacts"
    zips.mkdir()
    for role in ARTIFACT_WORKFLOWS:
        entry = record["artifacts"][role]
        if not isinstance(entry, dict):
            raise ValueError(f"{role} artifact record is incomplete")
        artifact_id = _positive(entry.get("artifact_id"), f"{role} artifact ID")
        _download_checked(artifact_id, _digest(entry.get("zip_sha256"), f"{role} ZIP digest"),
                          zips / f"{role}.zip", download, f"{role} artifact")
    verify(record, root, zips, record_dir / "evidence", record_dir / "reviews", github_json,
           min_remaining=PUBLICATION_REMAINING_RETENTION)
    for role in RELEASE_ROLES:
        _extract_verified(zips / f"{role}.zip", record["artifacts"][role]["files"], output / role)
    version = record["source"]["version"]
    (output / f"donner-{version}.candidate.json").write_bytes(raw)
    preflight = record["artifacts"]["source"]
    return {"preflight_run_id": str(preflight["run_id"]),
            "preflight_attempt": str(preflight["attempt"]), "version": version,
            "record_sha256": record_sha256}


# The docs deploy job's outputs, in either selection mode.
DOCS_SELECTION_KEYS = ("artifact_id", "run_id", "attempt", "source_commit", "artifact_digest",
                       "archive_sha256")
# The rollback selection's environment, validated by `pages_artifact.selection_from_environment`.
DOCS_ROLLBACK_ENVIRONMENT = ("DOC_ARTIFACT_ID", "DOC_SOURCE_RUN_ID", "DOC_SOURCE_ATTEMPT",
                             "DOC_SOURCE_COMMIT", "DOC_ARTIFACT_DIGEST", "DOC_ARCHIVE_SHA256")


def _assembly_record_id(run_id: int, attempt: int, github_json) -> tuple[int, str]:
    """Return the record artifact ID and commit of one successful manual assembly on main."""
    run = github_json(f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    if (not isinstance(run, dict) or run.get("id") != run_id or
            run.get("run_attempt") != attempt or
            run.get("repository", {}).get("full_name") != REPOSITORY or
            run.get("path") != ASSEMBLY_WORKFLOW or run.get("event") != "workflow_dispatch" or
            run.get("head_branch") != "main" or run.get("status") != "completed" or
            run.get("conclusion") != "success"):
        raise ValueError("candidate record must come from a successful manual assembly on main")
    commit = _commit(run.get("head_sha"), "assembly commit")
    name = f"{RECORD_ARTIFACT_PREFIX}{attempt}"
    listing = github_json(
        f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100&name={name}")
    found = listing.get("artifacts") if isinstance(listing, dict) else None
    matches = [item for item in found if isinstance(item, dict) and item.get("name") == name] \
        if isinstance(found, list) else []
    if len(matches) != 1:
        raise ValueError("assembly attempt must retain exactly one candidate record artifact")
    return _positive(matches[0].get("id"), "candidate record artifact ID"), commit


def docs_record(*, record_run_id: int, record_attempt: int, record_sha256: str, work: Path,
                github_json=_github_json, download=_download_artifact) -> dict[str, str]:
    """Download the record of one successful manual assembly attempt for a docs deploy.

    Only the record artifact is fetched and its digest checked; nothing it names is used yet. The
    deploy job verifies the record's attestation before `docs_selection` reads its contents.
    """
    _digest(record_sha256, "candidate record digest")
    _empty_directory(work, "docs record work")
    artifact_id, commit = _assembly_record_id(record_run_id, record_attempt, github_json)
    # A rollback may reselect this record after its publication window; it only must exist.
    _, _, record_dir = _fetch_record(artifact_id, record_run_id, record_attempt, commit,
                                     record_sha256, "dispatch input", work, github_json,
                                     download, timedelta(0))
    return {"record_path": str(record_dir / RECORD_NAME), "record_commit": commit}


def docs_selection(*, record_path: Path, record_commit: str, record_sha256: str, work: Path,
                   github_json=_github_json, download=_download_artifact) -> dict[str, str]:
    """Select the docs build that an attested candidate record names, for Pages promotion.

    `record_path` and `record_commit` come from `docs_record`. The `docs-provenance` artifact is
    downloaded and verified against the record; the `docs` entry's IDs and digests become the
    deployment selection, which the deploy job then checks against GitHub and the downloaded
    `artifact.tar` before promoting it.
    """
    commit = _commit(record_commit, "assembly commit")
    record = load_record(_read_bounded(record_path, MAX_RECORD_BYTES, "candidate record"),
                         record_sha256)
    if record["source"]["commit"] != commit:
        raise ValueError("candidate record names another commit than its assembly run")
    _empty_directory(work, "docs selection work")
    docs = record["artifacts"]["docs"]
    provenance = record["artifacts"]["docs-provenance"]
    if not isinstance(docs, dict) or not isinstance(provenance, dict) or \
            (docs.get("run_id"), docs.get("attempt")) != \
            (provenance.get("run_id"), provenance.get("attempt")):
        raise ValueError("docs provenance must come from the docs build attempt")
    now = datetime.now(timezone.utc)
    _check_artifact_entry("docs", docs, record, github_json, now, timedelta(0))
    fetch_role(record, "docs-provenance", NO_SOURCE_CHECKOUT, work / "docs-provenance.zip",
               work / "docs-provenance", github_json, download)
    return {"artifact_id": str(docs["artifact_id"]), "run_id": str(docs["run_id"]),
            "attempt": str(docs["attempt"]), "source_commit": commit,
            "artifact_digest": "sha256:" + docs["zip_sha256"],
            "archive_sha256": docs["files"]["artifact.tar"], "record_sha256": record_sha256}


def _docs_selection_command(args: argparse.Namespace, environment: dict[str, str]) -> dict:
    """Resolve the deploy selection: an attested candidate record, or a recorded rollback build."""
    rollback_values = [environment.get(name, "") for name in DOCS_ROLLBACK_ENVIRONMENT]
    record_values = (args.record_path, args.record_commit, args.record_sha256)
    if args.selection == "record":
        if any(rollback_values):
            raise ValueError("a record selection must not also name rollback values")
        if not all(record_values):
            raise ValueError("a record selection needs the fetched record, its commit and digest")
        result = docs_selection(record_path=Path(args.record_path),
                                record_commit=args.record_commit,
                                record_sha256=args.record_sha256, work=args.work)
    elif args.selection == "rollback":
        if any(record_values) or args.record_run:
            raise ValueError("a rollback selection must not also name a candidate record")
        selected = pages_artifact.selection_from_environment()
        result = {key: str(selected[name]) for key, name in zip(
            DOCS_SELECTION_KEYS, ("artifact_id", "run_id", "attempt", "source_commit",
                                  "artifact_digest", "archive_sha256"))}
    else:
        raise ValueError("docs selection must be record or rollback")
    return {"selection": args.selection, **result}


def _run_reference(value: str, label: str) -> tuple[int, int]:
    match = RUN_REFERENCE.fullmatch(value)
    if not match:
        raise ValueError(f"{label} must be RUN_ID/ATTEMPT with positive decimal numbers")
    return int(match.group(1)), int(match.group(2))


def _decimal_id(value: str, label: str) -> int:
    if not DECIMAL_ID.fullmatch(value):
        raise ValueError(f"{label} must be a positive decimal number")
    return _positive(int(value), label)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("verify", help="verify a record and its local files")
    for name in ("record", "source-root", "artifacts", "evidence", "reviews"):
        check.add_argument(f"--{name}", type=Path, required=True)
    build = commands.add_parser("assemble", help="assemble and verify a record from named runs")
    build.add_argument("--commit", required=True)
    for name in (*ARTIFACT_RUN_INPUTS, *EVIDENCE_WORKFLOWS):
        build.add_argument(f"--{name}", required=True, metavar="RUN_ID/ATTEMPT")
    for name in sorted(REVIEW_NAMES):
        build.add_argument(f"--{name}-review-comment", required=True, metavar="COMMENT_ID")
    for name in ("source-root", "work", "output"):
        build.add_argument(f"--{name}", type=Path, required=True)
    stage = commands.add_parser("release", help="re-verify a record and stage its release files")
    for name in ("record-run-id", "record-attempt", "record-artifact-id", "record-sha256"):
        stage.add_argument(f"--{name}", required=True)
    for name in ("source-root", "work", "output"):
        stage.add_argument(f"--{name}", type=Path, required=True)
    stage.add_argument("--github-output")
    fetch = commands.add_parser("docs-record", help="download a record for a docs deploy")
    fetch.add_argument("--record-run", required=True, metavar="RUN_ID/ATTEMPT")
    fetch.add_argument("--record-sha256", required=True)
    fetch.add_argument("--work", type=Path, required=True)
    fetch.add_argument("--github-output")
    docs = commands.add_parser("docs-selection",
                               help="select the docs build a record names, or a rollback build")
    docs.add_argument("--selection", required=True, choices=("record", "rollback"))
    docs.add_argument("--record-run", default="", metavar="RUN_ID/ATTEMPT",
                      help="only checked to be empty for a rollback selection")
    docs.add_argument("--record-path", default="")
    docs.add_argument("--record-commit", default="")
    docs.add_argument("--record-sha256", default="")
    docs.add_argument("--work", type=Path, required=True)
    docs.add_argument("--github-output")
    return parser


def _verify_command(args: argparse.Namespace) -> bytes:
    if args.record.is_symlink() or args.record.stat().st_size > MAX_RECORD_BYTES:
        raise ValueError("candidate record is linked or too large")
    raw = args.record.read_bytes()
    verify(_load_canonical(raw), args.source_root, args.artifacts, args.evidence, args.reviews)
    return raw


def _assemble_command(args: argparse.Namespace) -> bytes:
    runs = {name: _run_reference(getattr(args, name), f"{name} run")
            for name in (*ARTIFACT_RUN_INPUTS, *EVIDENCE_WORKFLOWS)}
    reviews = {name: _decimal_id(getattr(args, f"{name.replace('-', '_')}_review_comment"),
                                 f"{name} review comment ID")
               for name in REVIEW_NAMES}
    return assemble(commit=args.commit, runs=runs, reviews=reviews, root=args.source_root,
                    work=args.work, output=args.output)


def _release_command(args: argparse.Namespace) -> str:
    result = release(
        root=args.source_root,
        record_run_id=_decimal_id(args.record_run_id, "record run ID"),
        record_attempt=_decimal_id(args.record_attempt, "record attempt"),
        record_artifact_id=_decimal_id(args.record_artifact_id, "record artifact ID"),
        record_sha256=args.record_sha256, work=args.work, output=args.output)
    bcr_source.outputs(result, args.github_output)
    return result["record_sha256"]


def _docs_command(args: argparse.Namespace) -> str:
    result = _docs_selection_command(args, dict(os.environ))
    bcr_source.outputs(result, args.github_output)
    return result.get("record_sha256", "")


def _docs_record_command(args: argparse.Namespace) -> str:
    run_id, attempt = _run_reference(args.record_run, "record run")
    result = docs_record(record_run_id=run_id, record_attempt=attempt,
                         record_sha256=args.record_sha256, work=args.work)
    bcr_source.outputs(result, args.github_output)
    return args.record_sha256


def main() -> None:
    args = _parser().parse_args()
    commands = {"verify": _verify_command, "assemble": _assemble_command,
                "release": _release_command, "docs-selection": _docs_command,
                "docs-record": _docs_record_command}
    try:
        outcome = commands[args.command](args)
    except ValueError as error:
        raise SystemExit(f"Candidate verification failed: {error}") from None
    except (OSError, KeyError, TypeError, AttributeError, subprocess.CalledProcessError,
            zipfile.BadZipFile) as error:
        raise SystemExit(f"Candidate verification failed: {type(error).__name__}") from None
    if args.command == "docs-selection":
        print(f"Selected the {args.selection} docs build")
        return
    if args.command == "docs-record":
        print(f"Fetched release candidate sha256:{outcome}; verify its attestation before use")
        return
    digest = outcome if isinstance(outcome, str) else hashlib.sha256(outcome).hexdigest()
    print(f"Verified release candidate sha256:{digest}")


if __name__ == "__main__":
    main()
