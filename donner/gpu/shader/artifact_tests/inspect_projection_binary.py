#!/usr/bin/env python3
"""Checks selected and excluded shader payloads in a linked executable."""

import argparse
import pathlib
import sys


MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce"}
ELF_MAGIC = b"\x7fELF"
SPIRV_HEADER = b"\x03\x02\x23\x07\x00\x03\x01\x00\x00\x00\x00\x00"
PAYLOAD_MARKERS = {
    "slug_gradient": (b"struct GradientUniforms", b"donner_msl_member_radialFocalRadius"),
    "filter_blend": (b"fn blend_soft_light_channel", b"donner_msl_member_mode"),
    "gaussian": (b"struct BlurParams", b"donner_msl_member_stdDeviation"),
    "convolve": (b"struct ConvolveMatrixParams", b"donner_msl_member_coefficients"),
    "filter_resolve": (b"const kTransferCount:", b"donner_msl_member_userX0"),
    "offset": (b"struct OffsetParams", b"donner_msl_member_dx"),
    "specular_lighting": (b"struct LightingParams", b"donner_msl_member_specularExponent"),
    "slug_fill": (b"struct InstanceRecord", b"donner_msl_member_clipRectActive"),
    "image_blit": (b"fn sample_pixelated(", b"donner_msl_member_pixelatedScale"),
    "turbulence": (b"struct TurbulenceParams", b"donner_msl_member_stitchTiles"),
    "slug": (b"fn effective_bounding_vertex(", b"donner_msl_member_boundingVertexCount"),
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
