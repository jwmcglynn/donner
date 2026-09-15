#!/usr/bin/env python3
"""Assemble a self-contained Donner SVG Editor application and zip archive."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def load_paths(output: str, commands: set[str]) -> list[str]:
    """Read paths from matching Mach-O load commands, excluding install IDs."""
    result = []
    command = ""
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line[4:]
        elif command in commands and line.startswith(("name ", "path ")):
            result.append(line.split(" ", 1)[1].rsplit(" (offset ", 1)[0])
            command = ""
    return result


def dependencies(binary: Path) -> list[str]:
    return load_paths(run("/usr/bin/otool", "-l", str(binary)), {
        "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
        "LC_LAZY_LOAD_DYLIB", "LC_LOAD_UPWARD_DYLIB",
    })


def system_library(path: str) -> bool:
    return path.startswith(("/usr/lib/", "/System/Library/"))


def rpaths(binary: Path) -> list[str]:
    return load_paths(run("/usr/bin/otool", "-l", str(binary)), {"LC_RPATH"})


def relocate(binary: Path, runtimes: dict[str, Path], executable: bool) -> None:
    for dep in dependencies(binary):
        if system_library(dep):
            continue
        name = Path(dep).name
        if name not in runtimes:
            raise ValueError(f"Unbundled runtime dependency: {dep}")
        replacement = ("@executable_path/../Frameworks/" if executable else "@loader_path/") + name
        subprocess.run(["/usr/bin/install_name_tool", "-change", dep, replacement, str(binary)], check=True)
    for path in rpaths(binary):
        subprocess.run(["/usr/bin/install_name_tool", "-delete_rpath", path, str(binary)], check=True)
    if not executable:
        subprocess.run(["/usr/bin/install_name_tool", "-id", "@rpath/" + binary.name, str(binary)], check=True)


def archive_app(app: Path, output: Path) -> None:
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(app.rglob("*")):
            if path.is_dir() and not path.is_symlink():
                continue
            relative = path.relative_to(app.parent).as_posix()
            info = zipfile.ZipInfo(relative, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            if path.is_symlink():
                target = os.readlink(path)
                if os.path.isabs(target) or not path.resolve().is_relative_to(app.resolve()):
                    raise ValueError(f"Bundle symlink escapes the app: {relative}")
                info.external_attr = 0o120777 << 16
                data = target.encode()
            else:
                mode = 0o100755 if path.stat().st_mode & 0o111 else 0o100644
                info.external_attr = mode << 16
                data = path.read_bytes()
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def build(args: argparse.Namespace) -> None:
    if sys.platform != "darwin":
        raise ValueError("Application assembly requires macOS tools")
    output = args.output.resolve()
    if output.exists():
        raise ValueError("Refusing to replace an existing output archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.status_file:
        status = dict(line.split(" ", 1) for line in args.status_file.read_text().splitlines() if " " in line)
        args.source_revision = status.get("STABLE_DONNER_EDITOR_SOURCE_REVISION", "")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_revision or ""):
        raise ValueError("Source revision must be a complete Git commit ID; use "
                         "--workspace_status_command=tools/editor_workspace_status.sh with Bazel")
    version_match = re.search(r'^\s*version\s*=\s*"([^"]+)"', args.module.read_text(), re.MULTILINE)
    if not version_match:
        raise ValueError("MODULE.bazel must declare the package version")
    version = version_match[1]
    input_files = {name: getattr(args, name) for name in
                   ("editor", "icon", "agents", "skill", "vectorization", "module")}
    runtimes = {}
    for value in args.runtime:
        path = value.resolve()
        if path.name in runtimes and runtimes[path.name] != path:
            raise ValueError(f"Ambiguous runtime name: {path.name}")
        runtimes[path.name] = path
        input_files["runtime/" + path.name] = path
    with tempfile.TemporaryDirectory(prefix="donner-app-", dir=output.parent) as temp:
        app = Path(temp) / "Donner SVG Editor.app"
        macos = app / "Contents/MacOS"
        resources = app / "Contents/Resources"
        frameworks = app / "Contents/Frameworks"
        for directory in (macos, resources, frameworks):
            directory.mkdir(parents=True)
        binary = macos / "DonnerSVGEditor"
        shutil.copy2(args.editor, binary)
        binary.chmod(0o755)
        for name, source in runtimes.items():
            target = frameworks / name
            shutil.copy2(source, target)
            target.chmod(0o755)
            relocate(target, runtimes, False)
            subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(target)], check=True)
        relocate(binary, runtimes, True)
        skill = resources / "agent/skills/donner-editor"
        (skill / "references").mkdir(parents=True)
        shutil.copy2(args.agents, resources / "AGENTS.md")
        shutil.copy2(args.skill, skill / "SKILL.md")
        shutil.copy2(args.vectorization, skill / "references/vectorization.md")
        iconset = Path(temp) / "DonnerSVGEditor.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f"icon_{size}x{size}" + ("@2x" if scale == 2 else "") + ".png"
                subprocess.run([str(args.renderer), str(args.icon), "--width", str(size * scale),
                                "--height", str(size * scale), "--output", str(iconset / name)], check=True)
        subprocess.run(["/usr/bin/iconutil", "--convert", "icns", "--output",
                        str(resources / "DonnerSVGEditor.icns"), str(iconset)], check=True)
        info = {
            "CFBundleName": "Donner SVG Editor",
            "CFBundleDisplayName": "Donner SVG Editor",
            "CFBundleIdentifier": "org.donnersvg.editor",
            "CFBundleExecutable": "DonnerSVGEditor",
            "CFBundlePackageType": "APPL",
            "CFBundleIconFile": "DonnerSVGEditor.icns",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": args.build_number,
            "NSHighResolutionCapable": True,
            "LSApplicationCategoryType": "public.app-category.graphics-design",
            "CFBundleDocumentTypes": [{"CFBundleTypeName": "SVG artwork", "CFBundleTypeRole": "Editor",
                                       "LSItemContentTypes": ["public.svg-image"], "LSHandlerRank": "Alternate"}],
        }
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info, sort_keys=True))
        manifest = {
            "source_revision": args.source_revision,
            "version": version,
            "architecture": run("/usr/bin/lipo", "-archs", str(binary)),
            "signing": "ad-hoc development artifact",
            "input_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest()
                             for name, path in sorted(input_files.items())},
        }
        (resources / "build-provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["/usr/bin/codesign", "--verify", "--strict", "--deep", str(app)], check=True)
        for path in [binary, *frameworks.iterdir()]:
            for dep in dependencies(path):
                if not system_library(dep) and not dep.startswith(("@executable_path/../Frameworks/", "@loader_path/")):
                    raise ValueError(f"Nonportable dependency remains in {path.name}: {dep}")
        archive_app(app, output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    args.checksum.write_text(f"{digest}  {output.name}\n")
    print(f"Created {output.name} ({digest})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("editor", "renderer", "icon", "agents", "skill", "vectorization", "module", "output", "checksum"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--runtime", type=Path, action="append", default=[])
    parser.add_argument("--build-number", default="1")
    provenance = parser.add_mutually_exclusive_group(required=True)
    provenance.add_argument("--source-revision")
    provenance.add_argument("--status-file", type=Path)
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Cannot package editor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
