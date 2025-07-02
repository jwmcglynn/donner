#!/usr/bin/env python3
"""Embed resources into C++ source and header files."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict


def _sanitize_filename(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def _write_cpp(out: Path, variable: str, data: bytes) -> None:
    """
    Write binary data as a C++ array to a file.

    Args:
        out (Path): The output file path where the C++ code will be written
        variable (str): The variable name to use in the generated C++ code
        data (bytes): The binary data to embed in the C++ file
    """

    with out.open("w") as f:
        f.write(f"unsigned char __donner_embedded_{variable}[] = {{\n")
        for i, b in enumerate(data):
            if i % 12 == 0:
                f.write("  ")
            
            if i < len(data) - 1:
                f.write(f"0x{b:02x},")
                if i % 12 == 11:
                    f.write("\n")
                else:
                    f.write(" ")
            else:
                f.write(f"0x{b:02x}")
        if len(data) % 12 != 0:
            f.write("\n")
        f.write("};\n")
        f.write(f"unsigned int __donner_embedded_{variable}_len = {len(data)};\n")


def _write_header(out: Path, variables: Dict[str, Path]) -> None:
    """
    Writes a C++ header file that declares external variables for embedded resources.

    For each variable in the input dictionary, this function generates declarations for:
    - An external unsigned char array containing the embedded resource data
    - An external unsigned int with the length of the data
    - A std::span wrapper inside the donner::embedded namespace

    Args:
        out (Path): The output path where the header file will be written
        variables (Dict[str, Path]): Dictionary mapping variable names to their corresponding file paths
    """

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
    """
    Generate C++ source files for embedding binary resources and a corresponding header file.
    This function creates a directory structure and generates C++ files that contain the binary
    resources as arrays, along with a header file that declares these arrays.

    Args:
        out_dir (Path): The output directory where the generated files will be placed.
        header_name (str): The name of the header file to generate.
        resources (Dict[str, Path]): A dictionary mapping variable names to file paths.
                                    Each file path will be read and embedded as a binary array
                                    in a separate .cpp file, with the variable name as specified.
    """

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
