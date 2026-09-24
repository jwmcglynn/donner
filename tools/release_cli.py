#!/usr/bin/env python3
"""Package and verify CLI binaries built before a GitHub release is published."""

from __future__ import annotations

import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
import subprocess


PLATFORMS = {
    "linux-x86-64": "donner-svg_linux_x86_64",
    "darwin-arm64": "donner-svg_darwin_arm64",
}
INPUTS = (
    ".bazelversion",
    ".bazelrc",
    "MODULE.bazel",
    "MODULE.bazel.lock",
    "third_party/bazel/non_bcr_deps.bzl",
)
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
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


def _names(platform: str) -> tuple[str, str, str]:
    if platform not in PLATFORMS:
        raise ValueError("unsupported release CLI platform")
    binary = PLATFORMS[platform]
    return binary, f"{binary}.sha256", f"{binary}.provenance"


def _run_identity(run_id: str, attempt: str) -> None:
    if not POSITIVE_NUMBER.fullmatch(run_id) or not POSITIVE_NUMBER.fullmatch(attempt):
        raise ValueError("release CLI producer run and attempt must be positive integers")


def package(
    *, root: Path, binary: Path, output: Path, platform: str,
    commit: str, run_id: str, attempt: str,
) -> dict[str, object]:
    _run_identity(run_id, attempt)
    source_tree = _source_tree(root, commit)
    binary_name, checksum_name, provenance_name = _names(platform)
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError("release CLI binary is missing or empty")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("release CLI output directory must be empty")
    destination = output / binary_name
    shutil.copyfile(binary, destination)
    checksum = digest(destination)
    (output / checksum_name).write_text(f"{checksum}  {binary_name}\n", encoding="ascii")
    compiler = subprocess.check_output(["c++", "--version"], text=True).splitlines()[0]
    if not re.fullmatch(r"[A-Za-z0-9 .+()~_-]{1,256}", compiler):
        raise ValueError("host compiler version contains unsupported characters")
    provenance: dict[str, object] = {
        "schema": 1,
        "platform": platform,
        "source_commit": commit,
        "source_tree": source_tree,
        "producer_run_id": run_id,
        "producer_attempt": attempt,
        "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
        "host_cxx_version": compiler,
        "inputs_sha256": _input_digests(root),
        "binary_sha256": checksum,
        "binary_size": destination.stat().st_size,
    }
    (output / provenance_name).write_text(json.dumps(provenance, indent=2) + "\n")
    return provenance


def _check_provenance(provenance: object, expected: dict[str, object]) -> dict[str, object]:
    if not isinstance(provenance, dict):
        raise ValueError("release CLI provenance must be an object")
    for key, value in expected.items():
        if provenance.get(key) != value:
            raise ValueError("release CLI provenance does not match the selected candidate")
    compiler = provenance.get("host_cxx_version")
    if not isinstance(compiler, str) or not re.fullmatch(r"[A-Za-z0-9 .+()~_-]{1,256}", compiler):
        raise ValueError("release CLI compiler version is missing or invalid")
    if set(provenance) != set(expected) | {"host_cxx_version"}:
        raise ValueError("release CLI provenance has unexpected fields")
    return provenance


def verify(
    *, root: Path, artifacts: Path, platform: str,
    commit: str, run_id: str, attempt: str,
) -> dict[str, object]:
    _run_identity(run_id, attempt)
    source_tree = _source_tree(root, commit)
    binary_name, checksum_name, provenance_name = _names(platform)
    expected_names = {binary_name, checksum_name, provenance_name}
    if {path.name for path in artifacts.iterdir()} != expected_names:
        raise ValueError("release CLI artifact set is incomplete or contains unexpected files")
    if any((artifacts / name).is_symlink() for name in expected_names):
        raise ValueError("release CLI artifact set contains a symlink")
    binary = artifacts / binary_name
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError("release CLI binary is missing or empty")
    checksum = digest(binary)
    if (artifacts / checksum_name).read_text(encoding="ascii") != f"{checksum}  {binary_name}\n":
        raise ValueError("release CLI checksum does not match retained bytes")
    provenance = json.loads((artifacts / provenance_name).read_text(encoding="utf-8"))
    expected = {
        "schema": 1,
        "platform": platform,
        "source_commit": commit,
        "source_tree": source_tree,
        "producer_run_id": run_id,
        "producer_attempt": attempt,
        "bazel_version": (root / ".bazelversion").read_text(encoding="utf-8").strip(),
        "inputs_sha256": _input_digests(root),
        "binary_sha256": checksum,
        "binary_size": binary.stat().st_size,
    }
    return _check_provenance(provenance, expected)


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
            command.add_argument("--output", type=Path, required=True)
        else:
            command.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    options = dict(root=args.root, platform=args.platform, commit=args.commit,
                   run_id=args.run_id, attempt=args.attempt)
    if args.command == "package":
        package(binary=args.binary, output=args.output, **options)
    else:
        verify(artifacts=args.artifacts, **options)


if __name__ == "__main__":
    main()
