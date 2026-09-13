#!/usr/bin/env python3
"""Checks selected and excluded Gaussian shader payloads in a linked executable."""

import argparse
import pathlib
import sys


MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce"}
ELF_MAGIC = b"\x7fELF"
SPIRV_HEADER = b"\x03\x02\x23\x07\x00\x03\x01\x00\x00\x00\x00\x00"
PAYLOAD_MARKERS = {
    "gaussian": (b"struct BlurParams", b"donner_msl_member_stdDeviation"),
    "convolve": (b"struct ConvolveMatrixParams", b"donner_msl_member_coefficients"),
}


def require(payload: bytes, marker: bytes, present: bool, label: str) -> None:
    found = marker in payload
    if found != present:
        expected = "present" if present else "absent"
        raise AssertionError(f"expected {label} to be {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--family", choices=tuple(PAYLOAD_MARKERS), default="gaussian")
    parser.add_argument("--binary", required=True)
    parser.add_argument("--format", choices=("macho", "elf"), required=True)
    parser.add_argument("--wgsl", action="store_true")
    parser.add_argument("--msl", action="store_true")
    parser.add_argument("--spirv", action="store_true")
    args = parser.parse_args()

    payload = pathlib.Path(args.binary).read_bytes()
    if args.format == "macho" and payload[:4] not in MACHO_MAGICS:
        raise AssertionError("probe is not a Mach-O executable")
    if args.format == "elf" and payload[:4] != ELF_MAGIC:
        raise AssertionError("probe is not an ELF executable")
    wgsl_marker, msl_marker = PAYLOAD_MARKERS[args.family]
    require(payload, wgsl_marker, args.wgsl, "WGSL payload")
    require(payload, msl_marker, args.msl, "MSL payload")
    require(payload, SPIRV_HEADER, args.spirv, "SPIR-V header")
    print(f"{args.binary}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as error:
        print(f"projection binary inspection failed: {error}", file=sys.stderr)
        raise SystemExit(1)
