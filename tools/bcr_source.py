#!/usr/bin/env python3
"""Prepare and verify the immutable source archive used by BCR."""

from __future__ import annotations

import argparse
import ast
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?")
COMMIT = re.compile(r"[0-9a-f]{40}")
MAX_FILES = 100000
MAX_BYTES = 2 * 1024 * 1024 * 1024


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def module_values(contents: str) -> dict[str, object]:
    calls = [node.value for node in ast.parse(contents).body
             if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
             and isinstance(node.value.func, ast.Name) and node.value.func.id == "module"]
    if len(calls) != 1:
        raise ValueError("MODULE.bazel must contain one module declaration")
    values = {item.arg: ast.literal_eval(item.value) for item in calls[0].keywords}
    version = values.get("version", "")
    if values.get("name") != "donner" or not isinstance(version, str) or not VERSION.fullmatch(version):
        raise ValueError("expected a donner module with a release version")
    return values


def source_url(version: str) -> str:
    if not VERSION.fullmatch(version):
        raise ValueError("invalid module version")
    return f"https://github.com/jwmcglynn/donner/releases/download/v{version}/donner-{version}.tar.gz"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require_source_root_files(files: dict[str, tuple[str, int]]) -> None:
    if "MODULE.bazel" not in files:
        raise ValueError("source archive is missing MODULE.bazel")
    if "LICENSE" not in files or "NOTICE" not in files:
        raise ValueError("source archive is missing LICENSE or NOTICE")


def archive_members(archive: tarfile.TarFile, prefix: str) -> dict[str, tuple[str, int]]:
    files = {}
    seen = set()
    total = 0
    for member in archive:
        path = PurePosixPath(member.name)
        if (path.is_absolute() or ".." in path.parts or not path.parts
                or path.parts[0] != prefix or "\\" in member.name):
            raise ValueError("source archive contains an unsafe path")
        if str(path) in seen or len(seen) >= MAX_FILES:
            raise ValueError("source archive contains duplicate or excessive entries")
        seen.add(str(path))
        if member.isdir():
            continue
        if not member.isfile() or member.name != str(path):
            raise ValueError("source archive contains a link or special file")
        total += member.size
        if total > MAX_BYTES:
            raise ValueError("source archive exceeds the uncompressed size limit")
        name = str(PurePosixPath(*path.parts[1:]))
        with archive.extractfile(member) as stream:
            files[name] = (hashlib.file_digest(stream, "sha256").hexdigest(), member.mode)
    require_source_root_files(files)
    return files


def git_archive_manifest(commit: str, prefix: str) -> dict[str, tuple[str, int]]:
    with tempfile.TemporaryFile() as stream:
        subprocess.run(["git", "archive", "--format=tar", f"--prefix={prefix}/", commit],
                       stdout=stream, check=True)
        stream.seek(0)
        with tarfile.open(fileobj=stream) as archive:
            return archive_members(archive, prefix)


def consumer_calls(contents: str):
    for node in ast.parse(contents).body:
        if (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name)):
            yield node.value.func.id, {
                item.arg: ast.literal_eval(item.value) for item in node.value.keywords}


def verify_consumer(version: str) -> None:
    found = False
    for name, values in consumer_calls(Path("examples/bazel_consumer/MODULE.bazel").read_text()):
        if name.endswith("_override") and values.get("module_name") == "donner":
            raise ValueError("registry consumer must not override donner")
        if name == "bazel_dep" and values.get("name") == "donner":
            if values.get("version") != version:
                raise ValueError("consumer version does not match the source module")
            found = True
    if not found:
        raise ValueError("registry consumer must declare the donner version")


def verify_templates(version: str) -> None:
    source = json.loads(Path(".bcr/source.template.json").read_text())
    if source["url"].format(VERSION=version, TAG=f"v{version}") != source_url(version):
        raise ValueError("BCR source template must use the stable release asset URL")
    if source["strip_prefix"].format(VERSION=version) != f"donner-{version}":
        raise ValueError("BCR strip prefix does not match the archive")
    verify_consumer(version)


def create_archive(output: Path) -> dict[str, object]:
    root = Path(git("rev-parse", "--show-toplevel")).resolve()
    if root != Path.cwd().resolve() or output.resolve().is_relative_to(root):
        raise ValueError("run at the repository root and keep artifacts outside the checkout")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("source checkout must be clean before archiving")
    commit = git("rev-parse", "HEAD")
    values = module_values(git("show", f"{commit}:MODULE.bazel"))
    version = values["version"]
    verify_templates(version)
    output.mkdir(parents=True, exist_ok=False)
    prefix = f"donner-{version}"
    path = output / f"{prefix}.tar.gz"
    with tempfile.TemporaryFile() as raw:
        subprocess.run(["git", "archive", "--format=tar", f"--prefix={prefix}/", commit],
                       stdout=raw, check=True)
        raw.seek(0)
        with path.open("wb") as target, gzip.GzipFile(filename="", fileobj=target, mode="wb", mtime=0) as packed:
            shutil.copyfileobj(raw, packed)
    with tarfile.open(path) as archive:
        files = archive_members(archive, prefix)
    receipt = {
        "schema": 1, "source_commit": commit, "source_tree": git("rev-parse", "HEAD^{tree}"),
        "version": version, "archive": path.name, "sha256": digest(path),
        "files": len(files), "producer_run_id": os.environ.get("GITHUB_RUN_ID"),
        "producer_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
    }
    (output / f"{prefix}.provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    (output / f"{path.name}.sha256").write_text(f"{receipt['sha256']}  {path.name}\n")
    return receipt


def verify_artifact_files(directory: Path, prefix: str, receipt: dict) -> Path:
    path = directory / f"{prefix}.tar.gz"
    names = {path.name, f"{path.name}.sha256", f"{prefix}.provenance.json"}
    if {item.name for item in directory.iterdir()} != names or any(item.is_symlink() for item in directory.iterdir()):
        raise ValueError("source artifact contains unexpected files or symlinks")
    if digest(path) != receipt.get("sha256"):
        raise ValueError("source archive digest mismatch")
    if (directory / f"{path.name}.sha256").read_text() != f"{receipt['sha256']}  {path.name}\n":
        raise ValueError("source checksum file does not match the archive")
    return path


def verify_archive(directory: Path, commit: str, run_id: str | None = None,
                   attempt: str | None = None) -> dict[str, object]:
    if not COMMIT.fullmatch(commit):
        raise ValueError("source commit must be a full Git commit ID")
    version = module_values(git("show", f"{commit}:MODULE.bazel"))["version"]
    prefix = f"donner-{version}"
    receipt = json.loads((directory / f"{prefix}.provenance.json").read_text())
    expected = {"schema": 1, "version": version, "source_commit": commit,
                "source_tree": git("rev-parse", f"{commit}^{{tree}}"), "archive": f"{prefix}.tar.gz"}
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise ValueError("source archive provenance does not match the release source")
    if run_id is not None and (receipt.get("verified_run_id"), receipt.get("verified_run_attempt")) != (run_id, attempt):
        raise ValueError("source archive comes from a different CI run or attempt")
    path = verify_artifact_files(directory, prefix, receipt)

    with tarfile.open(path) as archive:
        actual = archive_members(archive, prefix)
    if actual != git_archive_manifest(commit, prefix) or len(actual) != receipt.get("files"):
        raise ValueError("source archive files or modes do not match Git")
    return receipt



def qualify_archive(directory: Path, commit: str, run_id: str, attempt: str) -> dict:
    receipt = verify_archive(directory, commit)
    producer_attempt = receipt.get("producer_run_attempt", "")
    if (receipt.get("producer_run_id") != run_id
            or not re.fullmatch(r"[1-9][0-9]*", run_id)
            or not re.fullmatch(r"[1-9][0-9]*", attempt)
            or not re.fullmatch(r"[1-9][0-9]*", str(producer_attempt))
            or int(producer_attempt) > int(attempt)):
        raise ValueError("source artifact does not belong to this CI run")
    receipt["verified_run_id"] = run_id
    receipt["verified_run_attempt"] = attempt
    path = directory / f"donner-{receipt['version']}.provenance.json"
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def prepare_registry(upstream: Path, output: Path, version: str) -> None:
    if not VERSION.fullmatch(version):
        raise ValueError("invalid registry version")
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(upstream / "bazel_registry.json", output / "bazel_registry.json")
    for module in (upstream / "modules").iterdir():
        if module.is_symlink():
            raise ValueError("upstream registry contains a module symlink")
        if module.is_dir() and module.name != "donner":
            (output / "modules" / module.name).mkdir(parents=True)
    copy_previous_versions(upstream / "modules/donner", output / "modules/donner", version)


def copy_previous_versions(old: Path, target: Path, version: str) -> None:
    if not old.exists():
        return
    if any(path.is_symlink() for path in old.rglob("*")):
        raise ValueError("upstream module metadata contains a symlink")
    target.mkdir(parents=True)
    for path in old.iterdir():
        if path.name == version:
            continue
        if path.is_dir():
            shutil.copytree(path, target / path.name)
        else:
            shutil.copyfile(path, target / path.name)
    metadata_path = target / "metadata.json"
    metadata = json.loads(metadata_path.read_text())
    metadata["versions"] = [item for item in metadata["versions"] if item != version]
    metadata["yanked_versions"].pop(version, None)
    metadata_path.write_text(json.dumps(metadata, indent=4) + "\n")


def prepare_consumer(artifacts: Path, registry: Path, output: Path, commit: str) -> None:
    receipt = verify_archive(artifacts, commit)
    version = receipt["version"]
    output.mkdir(parents=True, exist_ok=False)
    consumer = output / "consumer"
    consumer.mkdir()
    with tarfile.open(artifacts / receipt["archive"]) as archive:
        for name in ["MODULE.bazel", "BUILD.bazel", ".bazelrc", "main.cc"]:
            member = archive.getmember(f"donner-{version}/examples/bazel_consumer/{name}")
            (consumer / name).write_bytes(archive.extractfile(member).read())
    shutil.copytree(registry, output / "registry")
    path = output / f"registry/modules/donner/{version}/source.json"
    source = json.loads(path.read_text())
    source["url"] = (artifacts / receipt["archive"]).resolve().as_uri()
    path.write_text(json.dumps(source, indent=4) + "\n")


def outputs(values: dict[str, object], path: str | None) -> None:
    if path:
        with open(path, "a", encoding="utf-8") as stream:
            for key, value in values.items():
                if value is not None:
                    if "\n" in str(value) or "\r" in str(value):
                        raise ValueError("invalid workflow output")
                    stream.write(f"{key}={value}\n")
    print(json.dumps(values, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    archive = commands.add_parser("archive")
    archive.add_argument("--output", type=Path, required=True)
    archive.add_argument("--github-output")
    verify = commands.add_parser("verify")
    verify.add_argument("--artifacts", type=Path, required=True)
    verify.add_argument("--commit", required=True)
    verify.add_argument("--run-id")
    verify.add_argument("--attempt")
    qualify = commands.add_parser("qualify")
    qualify.add_argument("--artifacts", type=Path, required=True)
    qualify.add_argument("--commit", required=True)
    qualify.add_argument("--run-id", required=True)
    qualify.add_argument("--attempt", required=True)
    registry = commands.add_parser("registry")
    registry.add_argument("--upstream", type=Path, required=True)
    registry.add_argument("--output", type=Path, required=True)
    registry.add_argument("--version", required=True)
    consumer = commands.add_parser("consumer")
    consumer.add_argument("--artifacts", type=Path, required=True)
    consumer.add_argument("--registry", type=Path, required=True)
    consumer.add_argument("--output", type=Path, required=True)
    consumer.add_argument("--commit", required=True)
    args = parser.parse_args()
    if args.command == "archive":
        outputs(create_archive(args.output), args.github_output)
    elif args.command == "verify":
        outputs(verify_archive(args.artifacts, args.commit, args.run_id, args.attempt), None)
    elif args.command == "qualify":
        outputs(qualify_archive(args.artifacts, args.commit, args.run_id, args.attempt), None)
    elif args.command == "registry":
        prepare_registry(args.upstream, args.output, args.version)
    else:
        prepare_consumer(args.artifacts, args.registry, args.output, args.commit)


if __name__ == "__main__":
    main()
