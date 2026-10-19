#!/usr/bin/env python3
"""Package and verify CLI binaries built before a GitHub release is published."""

from __future__ import annotations

import argparse
from hashlib import sha256
import ipaddress
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlsplit


PLATFORMS = {
    "linux-x86-64": "donner-svg_linux_x86_64",
    "darwin-arm64": "donner-svg_darwin_arm64",
}
# The directory the release stages each platform's CLI files in, named by its candidate record
# role, in the order the release verifies them.
STAGED_PLATFORMS = {"cli-linux": "linux-x86-64", "cli-macos": "darwin-arm64"}
INPUTS = (
    ".bazelversion",
    ".bazelrc",
    "MODULE.bazel",
    "third_party/bazel/non_bcr_deps.bzl",
)
LOCKFILE = "MODULE.bazel.lock"
# Provenance schema 2 added the third-party license NOTICE that ships beside each binary.
# Schema 3 replaced `host_cxx_version` (the runner's `c++ --version`, which was not the compiler
# Bazel used) with `compiler_version`, reported by a probe built with the CLI.
SCHEMA = 3
NOTICE_HEADER = "Third-party licenses for build variant: "
MAX_NOTICE_BYTES = 1024 * 1024
PUBLIC_LOCK_HOSTS = frozenset({
    "bcr.bazel.build",
    "playwright.azureedge.net",
    "playwright-akamai.azureedge.net",
    "playwright-verizon.azureedge.net",
    "static.rust-lang.org",
    "github.com",
    "raw.githubusercontent.com",
    "nodejs.org",
    "storage.googleapis.com",
    "commondatastorage.googleapis.com",
    # Public package registries that module extensions in the production module graph record,
    # which the docs build retains (rules_python, rules_fuzzing, and the Bazel mirror).
    "files.pythonhosted.org",
    "mirror.bazel.build",
    "repo1.maven.org",
})
# An absolute host path starts the string or follows a separator; inside a Bazel label or a
# relative path, such as `@@rules_fuzzing+//fuzzing/private/oss_fuzz:repository.bzl`, the same
# directory name is not a host path.
PRIVATE_PATH = re.compile(
    r"(?i)(?:file://|(?<![A-Za-z0-9_.+@~/-])/(?:Users|home|private|var|tmp|opt|root|etc|mnt|proc|"
    r"srv|run)/|(?<![A-Za-z])[A-Za-z]:[\\/])")
SECRET_KEY = re.compile(r"(?i)(?:token|password|secret|credential|authorization|api[_-]?key|private[_-]?key)")
SECRET_SHAPE = re.compile(
    r"(?i)(?:-----BEGIN|AKIA[0-9A-Z]{16}|github_pat_[A-Za-z0-9_]{16,}|"
    r"gh[pousr]_[A-Za-z0-9]{20,}|\bBearer\s+\S+|"
    r"(?:token|password|secret|credential|authorization|api[_-]?key|private[_-]?key)\s*[:=])"
)
URI = re.compile(r"(?i)[A-Za-z][A-Za-z0-9+.-]*://[^\s\"'<>]+")
IP_ADDRESS = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
PRIVATE_HOST = re.compile(r"(?i)(?:\blocalhost\b|\b(?:internal|private|corp)\.[A-Za-z0-9.-]+|\b[A-Za-z0-9.-]+\.(?:local|lan|internal)\b)")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
COMPILER_VERSION = re.compile(r"[A-Za-z0-9 .+()~_-]{1,256}\Z")
COMPILER_PROBE_TIMEOUT_SECONDS = 60
POSITIVE_NUMBER = re.compile(r"[1-9][0-9]*\Z")


def digest(path: Path) -> str:
    checksum = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def _source_tree(root: Path, commit: str) -> str:
    if not COMMIT.fullmatch(commit):
        raise ValueError("source commit must be a full Git SHA")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if head != commit:
        raise ValueError("checkout does not match the selected source commit")
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD^{tree}"], cwd=root, text=True
    ).strip()


def _input_digests(root: Path) -> dict[str, str]:
    return {name: digest(root / name) for name in INPUTS}


def _checked_items(mapping: dict):
    for key, item in mapping.items():
        if SECRET_KEY.search(key):
            raise ValueError("generated Bazel lockfile contains a sensitive key")
        yield key
        yield item


def _lock_strings(document: dict):
    pending: list[object] = [document]
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            pending.extend(_checked_items(value))
        elif isinstance(value, list):
            pending.extend(value)
        elif isinstance(value, str):
            yield value


def _has_private_ip(value: str) -> bool:
    for match in IP_ADDRESS.finditer(value):
        try:
            address = ipaddress.ip_address(match.group())
        except ValueError:
            continue
        if address.is_private or address.is_loopback or address.is_link_local:
            return True
    return False


def _validate_public_uri(value: str) -> None:
    parsed = urlsplit(value)
    if (parsed.scheme.lower() != "https" or parsed.hostname not in PUBLIC_LOCK_HOSTS
            or parsed.username or parsed.password or parsed.query or parsed.fragment):
        raise ValueError("generated Bazel lockfile contains an unreviewed URL")


def _validate_lock_string(value: str) -> None:
    if (PRIVATE_PATH.search(value) or PRIVATE_HOST.search(value) or SECRET_SHAPE.search(value)
            or _has_private_ip(value) or "\x00" in value):
        raise ValueError("generated Bazel lockfile contains non-public content")
    for match in URI.finditer(value):
        _validate_public_uri(match.group())


def _validate_public_lockfile(path: Path) -> None:
    """Reject private runner details before a generated lock reaches public artifacts."""
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("generated Bazel lockfile must be a JSON object")
    for value in _lock_strings(document):
        _validate_lock_string(value)


MAX_LOCKFILE_BYTES = 16_000_000


def check_public_lockfile(path: Path) -> str:
    """Return the SHA-256 of a generated lockfile that may be retained in a public artifact.

    The editor package and the docs build retain their generated `MODULE.bazel.lock` beside their
    provenance, as each CLI does, so every published artifact records its resolved module graph.
    """
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_LOCKFILE_BYTES:
        raise ValueError("generated Bazel lockfile is missing, linked, empty or too large")
    _validate_public_lockfile(path)
    return digest(path)


def _names(platform: str) -> tuple[str, str, str, str, str]:
    if platform not in PLATFORMS:
        raise ValueError("unsupported release CLI platform")
    binary = PLATFORMS[platform]
    return (binary, f"{binary}.sha256", f"{binary}.provenance", f"{binary}.bazel.lock",
            f"{binary}.NOTICE.txt")


def _check_notice(path: Path) -> None:
    """Require the generated license notice: a bounded, non-empty UTF-8 text file."""
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_NOTICE_BYTES:
        raise ValueError("release CLI license notice is missing, empty or too large")
    try:
        text = path.read_bytes().decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("release CLI license notice is not UTF-8 text") from None
    if not text.startswith(NOTICE_HEADER) or "\x00" in text or "\nPackage: " not in text:
        raise ValueError("release CLI license notice is not a generated third-party notice")


def compiler_version(probe: Path) -> str:
    """Return the compiler identity printed by the probe built beside the release CLI.

    `//tools/ci:release_compiler_identity` is compiled in the same Bazel invocation and
    configuration as the CLI, so it names the compiler of the C++ toolchain Bazel resolved for the
    released binary. Any doubt about that identity fails closed.
    """
    if probe.is_symlink() or not probe.is_file() or probe.stat().st_size == 0:
        raise ValueError("release CLI compiler probe is missing or empty")
    try:
        result = subprocess.run([str(probe.resolve())], capture_output=True, check=False,
                                timeout=COMPILER_PROBE_TIMEOUT_SECONDS)
    except (OSError, subprocess.TimeoutExpired):
        raise ValueError("release CLI compiler probe could not run") from None
    if result.returncode != 0:
        raise ValueError("release CLI compiler probe failed")
    try:
        output = result.stdout.decode("ascii")
    except UnicodeDecodeError:
        raise ValueError("release CLI compiler version is not ASCII") from None
    version = output.removesuffix("\n")
    if "\n" in version or not COMPILER_VERSION.fullmatch(version):
        raise ValueError("release CLI compiler version is missing or has unsupported characters")
    return version


def _run_identity(run_id: str, attempt: str) -> None:
    if not POSITIVE_NUMBER.fullmatch(run_id) or not POSITIVE_NUMBER.fullmatch(attempt):
        raise ValueError("release CLI producer run and attempt must be positive integers")


def package(
    *, root: Path, binary: Path, notice: Path, compiler_probe: Path, output: Path, platform: str,
    commit: str, run_id: str, attempt: str,
) -> dict[str, object]:
    _run_identity(run_id, attempt)
    source_tree = _source_tree(root, commit)
    binary_name, checksum_name, provenance_name, lock_name, notice_name = _names(platform)
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError("release CLI binary is missing or empty")
    _check_notice(notice)
    compiler = compiler_version(compiler_probe)
    lockfile = root / LOCKFILE
    if lockfile.is_symlink() or not lockfile.is_file() or lockfile.stat().st_size == 0:
        raise ValueError("generated Bazel lockfile is missing or empty")
    _validate_public_lockfile(lockfile)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("release CLI output directory must be empty")
    destination = output / binary_name
    shutil.copyfile(binary, destination)
    shutil.copyfile(lockfile, output / lock_name)
    _validate_public_lockfile(output / lock_name)
    shutil.copyfile(notice, output / notice_name)
    _check_notice(output / notice_name)
    checksum = digest(destination)
    lock_checksum = digest(output / lock_name)
    (output / checksum_name).write_text(f"{checksum}  {binary_name}\n", encoding="ascii")
    provenance: dict[str, object] = {
        "schema": SCHEMA,
        "platform": platform,
        "source_commit": commit,
        "source_tree": source_tree,
        "producer_run_id": run_id,
        "producer_attempt": attempt,
        "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
        "compiler_version": compiler,
        "inputs_sha256": _input_digests(root),
        "generated_lock_sha256": lock_checksum,
        "binary_sha256": checksum,
        "binary_size": destination.stat().st_size,
        "notice_sha256": digest(output / notice_name),
    }
    (output / provenance_name).write_text(json.dumps(provenance, indent=2) + "\n")
    return provenance


def _check_provenance(provenance: object, expected: dict[str, object]) -> dict[str, object]:
    if not isinstance(provenance, dict):
        raise ValueError("release CLI provenance must be an object")
    for key, value in expected.items():
        if provenance.get(key) != value:
            raise ValueError("release CLI provenance does not match the selected candidate")
    compiler = provenance.get("compiler_version")
    if not isinstance(compiler, str) or not COMPILER_VERSION.fullmatch(compiler):
        raise ValueError("release CLI compiler version is missing or invalid")
    if set(provenance) != set(expected) | {"compiler_version"}:
        raise ValueError("release CLI provenance has unexpected fields")
    return provenance


def verify(
    *, root: Path, artifacts: Path, platform: str,
    commit: str, run_id: str, attempt: str,
) -> dict[str, object]:
    _run_identity(run_id, attempt)
    source_tree = _source_tree(root, commit)
    binary_name, checksum_name, provenance_name, lock_name, notice_name = _names(platform)
    expected_names = {binary_name, checksum_name, provenance_name, lock_name, notice_name}
    if {path.name for path in artifacts.iterdir()} != expected_names:
        raise ValueError("release CLI artifact set is incomplete or contains unexpected files")
    if any((artifacts / name).is_symlink() for name in expected_names):
        raise ValueError("release CLI artifact set contains a symlink")
    binary = artifacts / binary_name
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError("release CLI binary is missing or empty")
    retained_lock = artifacts / lock_name
    if not retained_lock.is_file() or retained_lock.stat().st_size == 0:
        raise ValueError("retained Bazel lockfile is missing or empty")
    _validate_public_lockfile(retained_lock)
    retained_notice = artifacts / notice_name
    _check_notice(retained_notice)
    checksum = digest(binary)
    if (artifacts / checksum_name).read_text(encoding="ascii") != f"{checksum}  {binary_name}\n":
        raise ValueError("release CLI checksum does not match retained bytes")
    provenance = json.loads((artifacts / provenance_name).read_text(encoding="utf-8"))
    expected = {
        "schema": SCHEMA,
        "platform": platform,
        "source_commit": commit,
        "source_tree": source_tree,
        "producer_run_id": run_id,
        "producer_attempt": attempt,
        "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
        "inputs_sha256": _input_digests(root),
        "generated_lock_sha256": digest(retained_lock),
        "binary_sha256": checksum,
        "binary_size": binary.stat().st_size,
        "notice_sha256": digest(retained_notice),
    }
    return _check_provenance(provenance, expected)


def verify_release(*, root: Path, release: Path, commit: str, run_id: str,
                   attempt: str) -> None:
    """Verify every platform's staged CLI files, in `STAGED_PLATFORMS` order, before upload.

    `release` is the directory the release workflow staged the record's files in. Each platform
    is checked exactly as `verify` checks it; the first failure stops before the next platform.
    """
    for directory, platform in STAGED_PLATFORMS.items():
        verify(root=root, artifacts=release / directory, platform=platform, commit=commit,
               run_id=run_id, attempt=attempt)
        print(f"Verified the {platform} release CLI files in {release / directory}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("package", "verify"):
        command = commands.add_parser(name)
        command.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
        command.add_argument("--commit", required=True)
        command.add_argument("--run-id", required=True)
        command.add_argument("--attempt", required=True)
        command.add_argument("--root", type=Path, default=Path.cwd())
        if name == "package":
            command.add_argument("--binary", type=Path, required=True)
            command.add_argument("--notice", type=Path, required=True)
            command.add_argument("--compiler-probe", type=Path, required=True,
                                 help="//tools/ci:release_compiler_identity from the same build")
            command.add_argument("--output", type=Path, required=True)
        else:
            command.add_argument("--artifacts", type=Path, required=True)
    staged = commands.add_parser(
        "verify-release", help="verify every platform's CLI files staged for a release")
    staged.add_argument("--release", type=Path, required=True)
    staged.add_argument("--commit", required=True)
    staged.add_argument("--run-id", required=True)
    staged.add_argument("--attempt", required=True)
    staged.add_argument("--root", type=Path, default=Path.cwd())
    lockfile = commands.add_parser(
        "check-lockfile", help="validate a generated lockfile for a public artifact")
    lockfile.add_argument("--lockfile", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "check-lockfile":
        try:
            print(f"{check_public_lockfile(args.lockfile)}  {args.lockfile.name}")
        except (ValueError, OSError, UnicodeDecodeError) as error:
            raise SystemExit(f"Lockfile check failed: {error}") from None
        return
    if args.command == "verify-release":
        verify_release(root=args.root, release=args.release, commit=args.commit,
                       run_id=args.run_id, attempt=args.attempt)
        return
    options = dict(root=args.root, platform=args.platform, commit=args.commit,
                   run_id=args.run_id, attempt=args.attempt)
    if args.command == "package":
        package(binary=args.binary, notice=args.notice, compiler_probe=args.compiler_probe,
                output=args.output, **options)
    else:
        verify(artifacts=args.artifacts, **options)


if __name__ == "__main__":
    main()
