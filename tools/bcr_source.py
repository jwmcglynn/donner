#!/usr/bin/env python3
"""Prepare and verify the immutable source archive used by BCR."""

from __future__ import annotations

import argparse
import ast
import base64
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
# Generated reports that `.gitattributes` keeps out of the source archive.
EXCLUDED_FILES = ("docs/reports/coverage.zip",)


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
    if any(name in files for name in EXCLUDED_FILES):
        raise ValueError("source archive contains a generated report excluded from releases")


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


# BCR Preflight retains the entry it generated and admission-validated beside this report.
ADMISSION_REPORT = "admission.json"
MAX_ENTRY_FILE_BYTES = 1_000_000
# The upstream validator's exit codes for a valid entry: 42 means a BCR maintainer must review it.
ACCEPTED_ADMISSION_CODES = (0, 42)


def entry_paths(version: str) -> tuple[PurePosixPath, ...]:
    """The registry files one release adds or changes, relative to the registry root."""
    if not VERSION.fullmatch(version):
        raise ValueError("invalid module version")
    root = PurePosixPath("modules/donner")
    return (root / version / "MODULE.bazel", root / version / "presubmit.yml",
            root / version / "source.json", root / "metadata.json")


def expected_source_json(version: str, sha256: str) -> dict[str, str]:
    """The `source.json` that points BCR at the released archive with this digest."""
    return {
        "integrity": "sha256-" + base64.b64encode(bytes.fromhex(sha256)).decode(),
        "strip_prefix": f"donner-{version}",
        "url": source_url(version),
    }


def check_metadata(metadata: dict, template: dict, version: str) -> tuple[list[str], dict]:
    metadata = metadata.copy()
    template = template.copy()
    versions = metadata.pop("versions", None)
    yanked = metadata.pop("yanked_versions", None)
    template.pop("versions", None)
    expected_yanked = template.pop("yanked_versions", {})
    if metadata != template:
        raise ValueError("registry metadata has different project or maintainer fields")
    if (not isinstance(versions, list) or not all(isinstance(item, str) for item in versions)
            or len(set(versions)) != len(versions) or versions.count(version) != 1):
        raise ValueError("registry metadata has a missing or invalid version list")
    verify_yanked_metadata(yanked, expected_yanked, version)
    return versions, yanked


def verify_yanked_metadata(yanked, expected_yanked: dict, version: str) -> None:
    if (not isinstance(yanked, dict)
            or any(not isinstance(value, str) for value in yanked.values())
            or any(yanked.get(key) != value for key, value in expected_yanked.items())
            or yanked.get(version) != expected_yanked.get(version)):
        raise ValueError("existing registry metadata has different yanked versions")


HISTORY_KEYS = ("versions", "yanked_versions")
# A Bazel module version as BCR's `tools/registry.py` `Version` parses it: dot-separated release
# identifiers, an optional `-` prerelease, and a `+` build suffix that does not affect order.
# Unlike that `^...$` match, a trailing newline is rejected.
REGISTRY_VERSION = re.compile(r"([A-Za-z0-9.]+)(?:-([A-Za-z0-9.-]+))?(?:\+[A-Za-z0-9.-]+)?")


def _version_identifiers(text: str) -> tuple:
    parts = text.split(".")
    if not all(parts):
        raise ValueError("registry metadata has a missing or invalid version list")
    return tuple((0, int(part)) if part.isdigit() else (1, part) for part in parts)


def registry_version_key(version: str) -> tuple:
    """Sort key that orders module versions the way Bazel and BCR presubmit compare them.

    Identifiers compare numerically when all digits and as text otherwise, with numbers first.
    A release that is a prefix of another sorts first, and a prerelease sorts before its release.
    """
    match = REGISTRY_VERSION.fullmatch(version)
    if not match:
        raise ValueError("registry metadata has a missing or invalid version list")
    release, prerelease = match.groups()
    return (_version_identifiers(release),
            (1,) if prerelease is None else (0, _version_identifiers(prerelease)))


def add_registry_version(versions: list[str], version: str) -> list[str]:
    """`versions` with `version` added, in the ascending order BCR presubmit requires.

    The registry's own list must already be in that order; it is never reordered.
    """
    if versions != sorted(versions, key=registry_version_key):
        raise ValueError("registry metadata lists its versions out of order")
    return sorted([*versions, version], key=registry_version_key)


def derive_metadata(upstream: dict | None, template: dict, version: str) -> bytes:
    """The `metadata.json` a submission proposes, derived from the registry it targets.

    Preflight's copy freezes the registry history at preflight time. Submission instead adds
    this version to the target registry's current `versions`, in ascending version order, and
    carries its `yanked_versions` forward; every other field comes from
    `.bcr/metadata.template.json`. The result is checked with `check_metadata` against the
    template, so a changed upstream maintainer or project field still fails closed.
    """
    if upstream is None:
        derived = {**template, "versions": [version],
                   "yanked_versions": template.get("yanked_versions", {})}
    else:
        if not isinstance(upstream, dict) or not isinstance(upstream.get("versions"), list):
            raise ValueError("registry metadata has a missing or invalid version list")
        if version in upstream["versions"]:
            raise ValueError("the registry already lists this donner version")
        # The registry's own project and maintainer fields must still be the template's.
        check_metadata({**upstream, "versions": [*upstream["versions"], version]}, template,
                       version)
        derived = {**template, "versions": add_registry_version(upstream["versions"], version),
                   "yanked_versions": upstream.get("yanked_versions")}
    check_metadata(derived, template, version)
    return (json.dumps(derived, indent=4) + "\n").encode()


def check_validated_metadata(validated: dict, derived: dict, version: str) -> None:
    """Require preflight's metadata to agree with the derived file apart from registry history."""
    if (not isinstance(validated, dict) or
            {key: value for key, value in validated.items() if key not in HISTORY_KEYS} !=
            {key: value for key, value in derived.items() if key not in HISTORY_KEYS} or
            not isinstance(validated.get("versions"), list) or
            validated["versions"].count(version) != 1 or
            not isinstance(validated.get("yanked_versions"), dict) or
            version in validated["yanked_versions"]):
        raise ValueError("registry metadata differs from the admission-validated entry")


def entry_bytes(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ENTRY_FILE_BYTES:
        raise ValueError("validated registry entry file is missing, linked or too large")
    return path.read_bytes()


def _verify_admission_report(path: Path, version: str) -> None:
    try:
        report = json.loads(entry_bytes(path))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("registry entry admission report is not JSON") from None
    results = report.get("results") if isinstance(report, dict) else None
    if (not isinstance(report, dict) or report.get("module") != f"donner@{version}" or
            type(report.get("upstream_exit_code")) is not int or
            report["upstream_exit_code"] not in ACCEPTED_ADMISSION_CODES or
            not isinstance(report.get("validator_revision"), str) or
            not COMMIT.fullmatch(report["validator_revision"]) or
            not isinstance(results, list) or not results or
            any(not isinstance(item, dict) or item.get("result") == "FAILED" for item in results)):
        raise ValueError("registry entry admission report is not a passing upstream validation "
                         "of this version")


def verify_validated_entry(directory: Path, source_root: Path, version: str, sha256: str) -> None:
    """Check the entry BCR Preflight generated and ran through the upstream BCR validator.

    The entry must name the approved archive digest, carry the source's exact module and
    presubmit files and the maintainer metadata of `.bcr/metadata.template.json`, and its
    retained admission report must be a passing upstream validation of this version.
    """
    _verify_admission_report(directory / ADMISSION_REPORT, version)
    module, presubmit, source, metadata = (directory / "registry" / path
                                           for path in entry_paths(version))
    try:
        source_json = json.loads(entry_bytes(source))
        metadata_json = json.loads(entry_bytes(metadata))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("validated registry entry has malformed JSON") from None
    if source_json != expected_source_json(version, sha256):
        raise ValueError("validated registry entry does not match the approved source archive")
    if (entry_bytes(module) != (source_root / "MODULE.bazel").read_bytes() or
            entry_bytes(presubmit) != (source_root / ".bcr/presubmit.yml").read_bytes()):
        raise ValueError("validated registry entry differs from the approved module files")
    template = json.loads((source_root / ".bcr/metadata.template.json").read_text(encoding="utf-8"))
    if not isinstance(metadata_json, dict):
        raise ValueError("registry metadata has different project or maintainer fields")
    check_metadata(metadata_json, template, version)


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
