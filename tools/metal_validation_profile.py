#!/usr/bin/env python3
"""Read the bounded native Metal validation profile marker from a Bazel test log."""

import argparse
from pathlib import Path
import sys


MARKERS = {
    "METAL_VALIDATION_PROFILE profile=full validation_state=enabled": "full",
    "METAL_VALIDATION_PROFILE profile=paravirtual-texture-usage-off validation_state=disabled":
        "paravirtual-texture-usage-off",
}


def parse_profile(text):
    markers = [line for line in text.splitlines() if line.startswith("METAL_VALIDATION_PROFILE ")]
    if len(markers) != 1 or markers[0] not in MARKERS:
        raise ValueError("Missing, ambiguous, or unsupported Metal validation profile")
    return MARKERS[markers[0]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_log", type=Path)
    args = parser.parse_args()
    if args.test_log.stat().st_size > 1024 * 1024:
        raise ValueError("Metal profile log exceeds the size bound")
    print(parse_profile(args.test_log.read_text(encoding="utf-8")))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
