#!/usr/bin/env python3
"""Prepare stripped native delivery bytes and retain their matching diagnostics."""

from __future__ import annotations

import argparse
from hashlib import sha256
import json
import os
import stat
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MAX_TOOL_BYTES = 1024 * 1024


def _tool(*arguments: str) -> str:
    with tempfile.TemporaryFile() as output:
        subprocess.run(arguments, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=120)
        if output.tell() > MAX_TOOL_BYTES:
            raise ValueError("native delivery tool output exceeds its limit")
        output.seek(0)
        return output.read().decode("utf-8")


def _digest(path: Path) -> str:
    return sha256(path.read_bytes()).hexdigest()


def _uuid(path: Path) -> str:
    values = re.findall(r"UUID: ([0-9A-Fa-f-]{36}) \(arm64\)",
                        _tool("/usr/bin/dwarfdump", "--uuid", str(path)))
    if len(values) != 1:
        raise ValueError("native delivery requires one arm64 Mach-O UUID")
    return values[0].lower()


def _build_id(path: Path) -> str:
    values = re.findall(r"Build ID: ([0-9a-f]+)", _tool("readelf", "-n", str(path)))
    if len(values) != 1:
        raise ValueError("native delivery requires one ELF build ID")
    return values[0]


def _mach_unwind(path: Path) -> dict[str, str]:
    contents = path.read_bytes()
    result = {}
    for section in _tool("/usr/bin/otool", "-l", str(path)).split("Section\n")[1:]:
        name = re.search(r"sectname (__eh_frame|__unwind_info)\s", section)
        size = re.search(r"\bsize (0x[0-9a-fA-F]+)", section)
        offset = re.search(r"\boffset ([0-9]+)", section)
        if name and size and offset:
            start, length = int(offset[1]), int(size[1], 16)
            if not length or start + length > len(contents):
                raise ValueError("native delivery unwind section is invalid")
            result[name[1]] = sha256(contents[start:start + length]).hexdigest()
    if not result:
        raise ValueError("native delivery Mach-O has no unwind information")
    return result


def _elf_unwind(path: Path) -> dict[str, str]:
    sections = _tool("readelf", "-SW", str(path))
    result = {}
    with tempfile.TemporaryDirectory() as temporary:
        for name in (".eh_frame", ".eh_frame_hdr", ".gcc_except_table"):
            if not re.search(re.escape(name) + r"\s", sections):
                continue
            output = Path(temporary) / "section"
            _tool("objcopy", "--dump-section", f"{name}={output}", str(path),
                  str(Path(temporary) / "inspected-image"))
            result[name] = _digest(output)
    if ".eh_frame" not in result:
        raise ValueError("native delivery ELF has no unwind information")
    return result


def _copy_dsym(source: Path, symbols: Path, identity: str) -> Path:
    if source.is_symlink() or not source.is_dir():
        raise ValueError("native delivery requires a Bazel-generated dSYM")
    if any(path.is_symlink() for path in source.rglob("*")):
        raise ValueError("native delivery dSYM contains a symlink")
    destination = symbols / "binary.dSYM"
    (destination / "Contents/Resources/DWARF").mkdir(parents=True)
    shutil.copyfile(source / "Contents/Info.plist", destination / "Contents/Info.plist")
    for path in (source / "Contents/Resources/DWARF").iterdir():
        if not path.is_file():
            raise ValueError("native delivery dSYM has unexpected debug entries")
        shutil.copyfile(path, destination / "Contents/Resources/DWARF" / path.name)
    dwarf_files = list((destination / "Contents/Resources/DWARF").glob("*"))
    if len(dwarf_files) != 1 or not dwarf_files[0].is_file() or _uuid(dwarf_files[0]) != identity:
        raise ValueError("native delivery dSYM UUID does not match the executable")
    _tool("/usr/bin/dwarfdump", "--verify", str(dwarf_files[0]))
    return dwarf_files[0]


def _check_inputs(binary: Path, output: Path, symbols: Path, platform: str,
                  dsym: Path | None) -> None:
    if platform not in ("darwin-arm64", "linux-x86-64"):
        raise ValueError("unsupported native delivery platform")
    if platform == "darwin-arm64" and dsym is None:
        raise ValueError("native delivery requires the source executable's dSYM")
    evidence = symbols.parent
    if (evidence.is_symlink() or not evidence.is_dir()
            or evidence.stat().st_uid != os.geteuid()
            or stat.S_IMODE(evidence.stat().st_mode) & 0o077):
        raise ValueError("native delivery symbols require an existing private evidence directory")
    if not binary.is_file() or os.path.lexists(output) or os.path.lexists(symbols):
        raise ValueError("native delivery needs an input and fresh owned output paths")


def _prepare_macos(binary: Path, output: Path, symbols: Path, dsym: Path,
                   sign: bool) -> tuple[str, dict, Path]:
    identity, before = _uuid(binary), _mach_unwind(binary)
    debug = _copy_dsym(dsym, symbols, identity)
    _tool("/usr/bin/strip", "-S", "-x", str(output))
    if sign:
        _tool("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(output))
        _tool("/usr/bin/codesign", "--verify", "--strict", str(output))
    if identity != _uuid(output) or before != _mach_unwind(output):
        raise ValueError("native delivery stripping changed identity or unwind data")
    return identity, before, debug


def _prepare_linux(binary: Path, output: Path, symbols: Path) -> tuple[str, dict, Path]:
    identity, before = _build_id(binary), _elf_unwind(binary)
    debug = symbols / "binary.debug"
    _tool("objcopy", "--only-keep-debug", str(binary), str(debug))
    if _build_id(debug) != identity:
        raise ValueError("native delivery debug build ID does not match the executable")
    _tool("strip", "--strip-unneeded", str(output))
    _tool("objcopy", f"--add-gnu-debuglink={debug}", str(output))
    if identity != _build_id(output) or before != _elf_unwind(output):
        raise ValueError("native delivery stripping changed identity or unwind data")
    return identity, before, debug


def _copy_fresh_executable(binary: Path, output: Path) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with binary.open("rb") as source:
        descriptor = os.open(output, flags, 0o755)
        with os.fdopen(descriptor, "wb") as destination:
            os.fchmod(destination.fileno(), 0o755)
            shutil.copyfileobj(source, destination)


def prepare(*, binary: Path, output: Path, symbols: Path, platform: str,
            dsym: Path | None = None, sign: bool = False) -> dict:
    """Strip a fresh copy while proving identity and unwind preservation against paired symbols."""
    _check_inputs(binary, output, symbols, platform, dsym)
    output.parent.mkdir(parents=True, exist_ok=True)
    symbols.mkdir(mode=0o700)
    _copy_fresh_executable(binary, output)
    if platform == "darwin-arm64":
        identity, unwind, debug = _prepare_macos(binary, output, symbols, dsym, sign)
    else:
        identity, unwind, debug = _prepare_linux(binary, output, symbols)
    if re.search(rb"/(?:Users|home)/[^/\x00]+/", debug.read_bytes()):
        raise ValueError("native delivery symbols contain an absolute private build path")
    metadata = {"schema": 1, "platform": platform, "identity": identity,
                "source_sha256": _digest(binary), "binary_sha256": _digest(output),
                "source_size": binary.stat().st_size, "binary_size": output.stat().st_size,
                "symbol_file": debug.relative_to(symbols).as_posix(),
                "symbol_sha256": _digest(debug), "unwind_sections_sha256": unwind}
    (symbols / "delivery.json").write_text(json.dumps(metadata, sort_keys=True, indent=2) + "\n")
    return metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "output", "symbols"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("darwin-arm64", "linux-x86-64"))
    parser.add_argument("--dsym", type=Path)
    parser.add_argument("--sign", action="store_true")
    prepare(**vars(parser.parse_args()))


if __name__ == "__main__":
    main()
