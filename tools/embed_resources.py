#!/usr/bin/env python3
"""Embed resources into C++ source and header files."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict


def _sanitize_filename(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def _write_cpp(out: Path, variable: str, data: bytes) -> None:
    with out.open("w") as f:
        f.write(f"unsigned char __donner_embedded_{variable}[] = {{\n")
        for i, b in enumerate(data):
            if i % 12 == 0:
                f.write("  ")
            f.write(f"0x{b:02x},")
            if i % 12 == 11:
                f.write("\n")
            else:
                f.write(" ")
        if len(data) % 12 != 0:
            f.write("\n")
        f.write("};\n")
        f.write(f"unsigned int __donner_embedded_{variable}_len = {len(data)};\n")


def _write_header(out: Path, variables: Dict[str, Path]) -> None:
    content = "#pragma once\n\n#include <span>\n\n"
    for var in variables:
        content += f"extern unsigned char __donner_embedded_{var}[];\n"
        content += f"extern unsigned int __donner_embedded_{var}_len;\n\n"
        content += "namespace donner::embedded {\n"
        content += (
            f"inline const std::span<const unsigned char> {var}("
            f"__donner_embedded_{var}, __donner_embedded_{var}_len);\n"
        )
        content += "}  // namespace donner::embedded\n\n"
    out.write_text(content)


def generate(out_dir: Path, header_name: str, resources: Dict[str, Path]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for var, path in resources.items():
        data = path.read_bytes()
        cpp = out_dir / f"{_sanitize_filename(path.name)}.cpp"
        _write_cpp(cpp, var, data)
    header = out_dir / header_name
    _write_header(header, resources)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--header", required=True)
    parser.add_argument("resources", nargs="+")
    args = parser.parse_args()

    res_map: Dict[str, Path] = {}
    for item in args.resources:
        if "=" not in item:
            parser.error(f"Invalid resource spec: {item}")
        var, src = item.split("=", 1)
        res_map[var] = Path(src)

    generate(args.out, args.header, res_map)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
