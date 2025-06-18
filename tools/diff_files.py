#!/usr/bin/env python3
"""Utility to diff two files and output a unified diff."""

from __future__ import annotations

import argparse
import difflib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two files and print a unified diff if they differ.")
    parser.add_argument("expected", type=Path)
    parser.add_argument("actual", type=Path)
    args = parser.parse_args()

    expected = args.expected
    actual = args.actual

    if expected.read_bytes() == actual.read_bytes():
        return 0

    diff = difflib.unified_diff(
        expected.read_text().splitlines(),
        actual.read_text().splitlines(),
        fromfile=str(expected),
        tofile=str(actual),
    )
    for line in diff:
        print(line)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
