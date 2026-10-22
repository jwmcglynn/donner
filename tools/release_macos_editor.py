#!/usr/bin/env python3
"""Retain an ad-hoc-signed macOS editor ZIP before release-candidate assembly."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import zipfile

from tools import release_cli


SCHEMA = 1
INPUTS = (*release_cli.INPUTS, "donner/editor/art/Info.plist", "tools/ci_macos_xcode.txt")
ARCHIVE = "donner-editor_darwin_arm64.zip"
NAMES = (ARCHIVE, ARCHIVE + ".sha256", ARCHIVE + ".provenance",
         ARCHIVE + ".bazel.lock", ARCHIVE + ".NOTICE.txt")
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_MEMBER_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_FILES = 2048
MAX_CONTROL_BYTES = 1024 * 1024
BUNDLE = "Donner SVG Editor.app"
REQUIRED = {"LICENSE", "NOTICE.txt", "Donner SVG Editor.app/Contents/Info.plist",
            "Donner SVG Editor.app/Contents/MacOS/Donner", "Donner SVG Editor.app/Contents/Resources/Donner.icns",
            "Donner SVG Editor.app/Contents/_CodeSignature/CodeResources"}
NATIVE_CHECKS = {"architecture": "arm64", "signature": "ad-hoc",
                 "dependency_closure": "bundle-and-system", "zip_roundtrip": True,
                 "entrypoint": "invalid-option-exit-before-window"}


def _safe_name(name: str) -> None:
    path = PurePosixPath(name)
    if (not name or len(name) > 256 or not re.fullmatch(r"[A-Za-z0-9._/ -]+", name)
            or path.is_absolute() or any(part in ("", ".", "..") or part != part.strip() for part in name.split("/"))
            or (name not in {"LICENSE", "NOTICE.txt"} and not name.startswith(BUNDLE + "/"))):
        raise ValueError("native editor ZIP has an unsafe or unexpected path")


def _checked_info(item: zipfile.ZipInfo) -> int:
    _safe_name(item.filename)
    mode = item.external_attr >> 16
    if (item.is_dir() or stat.S_IFMT(mode) != stat.S_IFREG
            or stat.S_IMODE(mode) not in (0o644, 0o755) or item.flag_bits & 1
            or item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
            or item.extra or item.comment or not 0 < item.file_size <= MAX_MEMBER_BYTES):
        raise ValueError("native editor ZIP has an unsupported entry or size")
    return stat.S_IMODE(mode)


def _check_zip_directory(path: Path) -> None:
    with path.open("rb") as stream:
        if stream.read(4) != b"PK\x03\x04":
            raise ValueError("native editor ZIP has an invalid local header")
        stream.seek(-22, 2)
        end = stream.read(22)
    fields = struct.unpack("<4s4H2IH", end)
    marker, disk, directory_disk, disk_count, count, size, offset, comment = fields
    if (marker != b"PK\x05\x06" or disk or directory_disk or disk_count != count
            or not 0 < count <= MAX_FILES or comment or size > MAX_FILES * 1024
            or offset + size != path.stat().st_size - 22):
        raise ValueError("native editor ZIP central directory exceeds its bounded contract")


def _check_prefix_case(name: str, prefixes: dict) -> None:
    parts = name.split("/")
    for index in range(1, len(parts) + 1):
        prefix = "/".join(parts[:index])
        prior = prefixes.setdefault(prefix.casefold(), prefix)
        if prior != prefix:
            raise ValueError("native editor ZIP has case-colliding directories")


def _admit_members(archive: zipfile.ZipFile) -> dict:
    files, prefixes = {}, {}
    folded = set()
    total = 0
    entries = archive.infolist()
    if not 0 < len(entries) <= MAX_FILES:
        raise ValueError("native editor ZIP has too many entries")
    for item in entries:
        mode = _checked_info(item)
        if item.filename.casefold() in folded:
            raise ValueError("native editor ZIP has duplicate or case-colliding entries")
        folded.add(item.filename.casefold())
        _check_prefix_case(item.filename, prefixes)
        total += item.file_size
        if total > MAX_TOTAL_BYTES:
            raise ValueError("native editor ZIP exceeds its expanded size limit")
        files[item.filename] = {"mode": mode, "size": item.file_size}
    if not REQUIRED <= files.keys():
        raise ValueError("native editor ZIP is missing required app or license files")
    for name in files:
        if any(str(parent).casefold() in folded for parent in PurePosixPath(name).parents):
            raise ValueError("native editor ZIP has conflicting file and directory paths")
    return files


def _member_digest(archive: zipfile.ZipFile, name: str, size: int) -> str:
    with archive.open(name) as stream:
        checksum = hashlib.sha256()
        count = 0
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            count += len(chunk)
            if count > size:
                raise ValueError("native editor ZIP member exceeds its declared size")
            checksum.update(chunk)
    if count != size:
        raise ValueError("native editor ZIP member differs from its declared size")
    return checksum.hexdigest()


def inspect_archive(path: Path) -> dict:
    """Validate bounded ZIP contents and return each retained file's digest and mode."""
    if path.is_symlink() or not path.is_file() or not 22 <= path.stat().st_size <= MAX_ARCHIVE_BYTES:
        raise ValueError("native editor ZIP is missing, linked, empty or too large")
    _check_zip_directory(path)
    with zipfile.ZipFile(path) as archive:
        files = _admit_members(archive)
        for name, record in files.items():
            record["sha256"] = _member_digest(archive, name, record["size"])
        if files["Donner SVG Editor.app/Contents/MacOS/Donner"]["mode"] != 0o755:
            raise ValueError("native editor executable has lost its executable mode")
        if files["Donner SVG Editor.app/Contents/Info.plist"]["size"] > MAX_CONTROL_BYTES:
            raise ValueError("native editor Info.plist is too large")
    return files


def _input_digests(root: Path) -> dict:
    return {name: release_cli.digest(root / name) for name in INPUTS}


def _check_app_metadata(root: Path, files: dict) -> None:
    expected = release_cli.digest(root / "donner/editor/art/Info.plist")
    if files["Donner SVG Editor.app/Contents/Info.plist"]["sha256"] != expected:
        raise ValueError("native editor Info.plist differs from the committed source")


def _tool(*args: str) -> str:
    with tempfile.TemporaryFile() as output:
        try:
            subprocess.run(args, check=True, stdout=output, stderr=subprocess.STDOUT, timeout=60)
        except (OSError, subprocess.SubprocessError):
            raise ValueError("native editor platform validation failed") from None
        if output.tell() > MAX_CONTROL_BYTES:
            raise ValueError("native editor platform validation output is too large")
        output.seek(0)
        return output.read().decode("utf-8")


def _expanded_path(value: str, image: Path, app: Path) -> Path:
    value = value.replace("@executable_path", str(app / "Contents/MacOS"))
    value = value.replace("@loader_path", str(image.parent))
    path = Path(value)
    resolved = path.resolve()
    code_roots = [app.resolve() / "Contents/MacOS", app.resolve() / "Contents/Frameworks"]
    if not path.is_absolute() or not any(resolved.is_relative_to(root) for root in code_roots):
        raise ValueError("native editor dependency escapes the application bundle")
    return resolved


def _rpaths(image: Path, app: Path) -> list[Path]:
    paths = []
    in_rpath = False
    for line in _tool("/usr/bin/otool", "-l", str(image)).splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd "):
            in_rpath = stripped == "cmd LC_RPATH"
        if in_rpath and stripped.startswith("path "):
            paths.append(_expanded_path(stripped[5:].split(" (offset ")[0], image, app))
    return paths


def _check_dependency(dependency: str, image: Path, app: Path, paths: list[Path]) -> None:
    if dependency.startswith(("/System/Library/", "/usr/lib/")):
        if ".." in PurePosixPath(dependency).parts:
            raise ValueError("native editor system dependency has a noncanonical path")
        return
    if dependency.startswith("@rpath/"):
        matches = [(base / dependency[7:]).resolve() for base in paths]
        if not any(path.is_file() and path.is_relative_to(app.resolve()) for path in matches):
            raise ValueError("native editor has an unresolved bundled dependency")
    elif not _expanded_path(dependency, image, app).is_file():
        raise ValueError("native editor has an unresolved bundled dependency")


def _check_dependencies(image: Path, app: Path, main_rpaths: list[Path]) -> None:
    paths = _rpaths(image, app) + main_rpaths
    for line in _tool("/usr/bin/otool", "-L", str(image)).splitlines()[1:]:
        dependency = line.strip().split(" (compatibility version ")[0]
        _check_dependency(dependency, image, app, paths)


def validate_native(app: Path) -> None:
    """Check the existing bundle's signatures, arm64 images and local dependency closure on macOS."""
    executable = app / "Contents/MacOS/Donner"
    _tool("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    signature = _tool("/usr/bin/codesign", "--display", "--verbose=4", str(app))
    if "Signature=adhoc" not in signature:
        raise ValueError("native editor signing identity differs from the ad-hoc contract")
    images = [executable, *sorted((app / "Contents/Frameworks").glob("*.dylib"))]
    main_rpaths = _rpaths(executable, app)
    for image in images:
        if _tool("/usr/bin/lipo", "-archs", str(image)).strip() != "arm64":
            raise ValueError("native editor image is not arm64")
        _check_dependencies(image, app, main_rpaths)


def _regular_size(path: Path) -> int:
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_MEMBER_BYTES:
        raise ValueError("native editor bundle has an invalid or linked file")
    return path.stat().st_size


def _bundle_files(app: Path, license_path: Path, notice: Path) -> list:
    files = [("LICENSE", license_path), ("NOTICE.txt", notice)]
    if app.is_symlink() or not app.is_dir():
        raise ValueError("native editor bundle is missing or linked")
    visited = 0
    total = _regular_size(license_path) + _regular_size(notice)
    for parent, directories, names in os.walk(app, followlinks=False):
        visited += 1 + len(names) + len(directories)
        if visited > MAX_FILES:
            raise ValueError("native editor bundle has too many entries")
        if any((Path(parent) / name).is_symlink() for name in directories):
            raise ValueError("native editor bundle contains a symlink")
        for name in names:
            path = Path(parent) / name
            total += _regular_size(path)
            if total > MAX_TOTAL_BYTES:
                raise ValueError("native editor bundle exceeds its expanded size limit")
            files.append((BUNDLE + "/" + str(path.relative_to(app)), path))
    return files


def _write_zip(app: Path, license_path: Path, notice: Path, output: Path) -> None:
    files = _bundle_files(app, license_path, notice)
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, source in sorted(files):
            _safe_name(name)
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            mode = 0o755 if source.stat().st_mode & 0o111 else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with source.open("rb") as reader, archive.open(info, "w") as writer:
                shutil.copyfileobj(reader, writer, length=1024 * 1024)


def _roundtrip(path: Path, files: dict) -> None:
    with tempfile.TemporaryDirectory(prefix="donner-editor-verify-") as temporary:
        root = Path(temporary)
        with zipfile.ZipFile(path) as archive:
            for name, record in files.items():
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(name) as reader, destination.open("xb") as writer:
                    shutil.copyfileobj(reader, writer, length=1024 * 1024)
                destination.chmod(record["mode"])
        validate_native(root / BUNDLE)
        _check_entrypoint(root / BUNDLE)


def _check_entrypoint(app: Path) -> None:
    option = "--donner-release-loader-check"
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("DYLD_", "RUNFILES_"))
                   and key not in {"BUILD_WORKING_DIRECTORY", "BUILD_WORKSPACE_DIRECTORY",
                                   "LD_LIBRARY_PATH", "JAVA_RUNFILES", "PYTHON_RUNFILES"}}
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run([str(app / "Contents/MacOS/Donner"), option],
                                    stdout=subprocess.DEVNULL, stderr=output, timeout=30,
                                    cwd=app.parent, env=environment)
        except (OSError, subprocess.SubprocessError):
            raise ValueError("native editor extracted entrypoint could not run") from None
        if output.tell() > MAX_CONTROL_BYTES:
            raise ValueError("native editor entrypoint output is too large")
        output.seek(0)
        expected = ("Unknown option " + option + "\nUsage: donner-editor ").encode()
        if result.returncode != 1 or not output.read(len(expected)).startswith(expected):
            raise ValueError("native editor extracted entrypoint did not reject the test option")


def _native_toolchain(root: Path) -> dict:
    pin = [line.strip() for line in (root / "tools/ci_macos_xcode.txt").read_text().splitlines()
           if line.strip() and not line.startswith("#")]
    xcode = _tool("/usr/bin/xcodebuild", "-version").strip().splitlines()
    if len(pin) != 1 or len(xcode) != 2 or xcode != ["Xcode " + pin[0].split()[0],
                                                                  "Build version " + pin[0].split()[1]]:
        raise ValueError("native editor Xcode differs from the source pin")
    sdk = _tool("/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-version").strip()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", sdk):
        raise ValueError("native editor SDK version is invalid")
    return {"xcode": pin[0], "macos_sdk": sdk}


def _check_toolchain(root: Path, value: object) -> None:
    pin = [line.strip() for line in (root / "tools/ci_macos_xcode.txt").read_text().splitlines()
           if line.strip() and not line.startswith("#")]
    if (not isinstance(value, dict) or set(value) != {"xcode", "macos_sdk"}
            or len(pin) != 1 or value["xcode"] != pin[0]
            or not isinstance(value["macos_sdk"], str)
            or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", value["macos_sdk"])):
        raise ValueError("native editor toolchain provenance differs from the source pin")


def package(*, root: Path, app: Path, notice: Path, compiler_probe: Path, output: Path,
            commit: str, run_id: str, attempt: str) -> dict:
    """Package existing signed bytes once; native checks run before and after ZIP roundtrip."""
    release_cli._run_identity(run_id, attempt)
    tree = release_cli._source_tree(root, commit)
    release_cli._check_notice(notice)
    lock = root / release_cli.LOCKFILE
    release_cli.check_public_lockfile(lock)
    compiler = release_cli.compiler_version(compiler_probe)
    toolchain = _native_toolchain(root)
    validate_native(app)
    if output.is_symlink():
        raise ValueError("native editor output directory must not be linked")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("native editor output directory must be empty")
    archive = output / ARCHIVE
    _write_zip(app, root / "LICENSE", notice, archive)
    files = inspect_archive(archive)
    _check_app_metadata(root, files)
    _roundtrip(archive, files)
    shutil.copyfile(lock, output / NAMES[3])
    shutil.copyfile(notice, output / NAMES[4])
    checksum = release_cli.digest(archive)
    (output / NAMES[1]).write_text(f"{checksum}  {ARCHIVE}\n", encoding="ascii")
    provenance = {"schema": SCHEMA, "platform": "darwin-arm64", "source_commit": commit,
                  "source_tree": tree, "producer_run_id": run_id, "producer_attempt": attempt,
                  "bazel_version": (root / ".bazelversion").read_text().strip(),
                  "compiler_version": compiler, "inputs_sha256": _input_digests(root),
                  "generated_lock_sha256": release_cli.digest(output / NAMES[3]),
                  "archive_sha256": checksum, "archive_size": archive.stat().st_size,
                  "notice_sha256": release_cli.digest(output / NAMES[4]),
                  "license_sha256": release_cli.digest(root / "LICENSE"),
                  "native_validation": NATIVE_CHECKS, "files": files, "toolchain": toolchain}
    (output / NAMES[2]).write_text(json.dumps(provenance, sort_keys=True, indent=2) + "\n")
    return provenance


def verify(*, root: Path, artifacts: Path, commit: str, run_id: str, attempt: str) -> dict:
    """Verify frozen ZIP, license and provenance bytes without claiming Linux runs native checks."""
    release_cli._run_identity(run_id, attempt)
    tree = release_cli._source_tree(root, commit)
    if {path.name for path in artifacts.iterdir()} != set(NAMES):
        raise ValueError("native editor artifact set differs from its contract")
    if any((artifacts / name).is_symlink() or not (artifacts / name).is_file() for name in NAMES):
        raise ValueError("native editor artifact set contains missing or linked files")
    archive = artifacts / ARCHIVE
    files = inspect_archive(archive)
    _check_app_metadata(root, files)
    release_cli._check_notice(artifacts / NAMES[4])
    lock_digest = release_cli.check_public_lockfile(artifacts / NAMES[3])
    checksum = release_cli.digest(archive)
    if (artifacts / NAMES[1]).stat().st_size > 1024 or (artifacts / NAMES[1]).read_text() != f"{checksum}  {ARCHIVE}\n":
        raise ValueError("native editor ZIP checksum differs from retained bytes")
    provenance_path = artifacts / NAMES[2]
    if provenance_path.stat().st_size > MAX_CONTROL_BYTES:
        raise ValueError("native editor provenance is too large")
    provenance = json.loads(provenance_path.read_text())
    _check_toolchain(root, provenance.get("toolchain"))
    expected = {"schema": SCHEMA, "platform": "darwin-arm64", "source_commit": commit,
                "source_tree": tree, "producer_run_id": run_id, "producer_attempt": attempt,
                "bazel_version": (root / ".bazelversion").read_text().strip(),
                "inputs_sha256": _input_digests(root),
                "generated_lock_sha256": lock_digest, "archive_sha256": checksum,
                "archive_size": archive.stat().st_size,
                "notice_sha256": release_cli.digest(artifacts / NAMES[4]),
                "license_sha256": release_cli.digest(root / "LICENSE"),
                "native_validation": NATIVE_CHECKS, "files": files,
                "toolchain": provenance["toolchain"]}
    result = release_cli._check_provenance(provenance, expected)
    for name, digest in (("LICENSE", expected["license_sha256"]),
                         ("NOTICE.txt", expected["notice_sha256"])):
        if files[name]["sha256"] != digest:
            raise ValueError("native editor ZIP license differs from its bound source or notice")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("package", "verify"):
        command = commands.add_parser(name)
        for flag in ("commit", "run-id", "attempt"):
            command.add_argument("--" + flag, required=True)
        command.add_argument("--root", type=Path, default=Path.cwd())
        if name == "package":
            for flag in ("app", "notice", "compiler-probe", "output"):
                command.add_argument("--" + flag, type=Path, required=True)
        else:
            command.add_argument("--artifacts", type=Path, required=True)
    args = vars(parser.parse_args())
    command = args.pop("command")
    if command == "package":
        package(**args)
    else:
        verify(**args)


if __name__ == "__main__":
    main()
