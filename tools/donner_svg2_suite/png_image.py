#!/usr/bin/env python3
"""Minimal 8-bit RGBA PNG codec for the reference runner's comparator.

The design requires a render adapter to return an 8-bit RGBA PNG and the runner
to compare it against the oracle PNG. To keep the pilot runner hermetic and
dependency-free (matching the repo's stdlib-only tooling), this module encodes
and decodes non-interlaced, 8-bit, colour-type-6 (RGBA) PNGs using only
``zlib`` and ``struct``. It is deliberately narrow: anything outside that
profile raises ``PngError`` rather than guessing.

Donner's production adapter is expected to reuse Donner's own pixel-comparison
implementation (design 0057, Milestone 2); this codec backs the portable
process-adapter path and the runner's tests.
"""

from __future__ import annotations

import struct
import zlib


_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PngError(ValueError):
    """Raised for malformed or unsupported PNG data."""


def _chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def encode_rgba(width: int, height: int, pixels: bytes) -> bytes:
    """Encode ``width`` x ``height`` RGBA pixels (row-major) into PNG bytes."""

    if width <= 0 or height <= 0:
        raise PngError("width and height must be positive")
    if len(pixels) != width * height * 4:
        raise PngError("pixel buffer size does not match dimensions")

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += pixels[y * stride : (y + 1) * stride]
    idat = zlib.compress(bytes(raw), 9)
    return _SIGNATURE + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")


def _unfilter(raw: bytes, width: int, height: int) -> bytes:
    stride = width * 4
    out = bytearray()
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        if position >= len(raw):
            raise PngError("truncated scanline data")
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position : position + stride])
        if len(line) != stride:
            raise PngError("truncated scanline")
        position += stride
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up = previous[i]
            up_left = previous[i - 4] if i >= 4 else 0
            if filter_type == 0:
                value = line[i]
            elif filter_type == 1:
                value = line[i] + left
            elif filter_type == 2:
                value = line[i] + up
            elif filter_type == 3:
                value = line[i] + ((left + up) >> 1)
            elif filter_type == 4:
                predictor = left + up - up_left
                pa = abs(predictor - left)
                pb = abs(predictor - up)
                pc = abs(predictor - up_left)
                if pa <= pb and pa <= pc:
                    paeth = left
                elif pb <= pc:
                    paeth = up
                else:
                    paeth = up_left
                value = line[i] + paeth
            else:
                raise PngError(f"unsupported filter type {filter_type}")
            line[i] = value & 0xFF
        out += line
        previous = line
    return bytes(out)


def decode_rgba(data: bytes) -> tuple[int, int, bytes]:
    """Decode PNG ``data`` into ``(width, height, rgba_pixels)``."""

    if data[:8] != _SIGNATURE:
        raise PngError("not a PNG file")
    position = 8
    width = height = None
    idat = bytearray()
    while position + 8 <= len(data):
        (length,) = struct.unpack(">I", data[position : position + 4])
        tag = data[position + 4 : position + 8]
        chunk_data = data[position + 8 : position + 8 + length]
        position += 12 + length
        if tag == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk_data[:10])
            if depth != 8 or color_type != 6:
                raise PngError("only 8-bit RGBA (color type 6) PNGs are supported")
        elif tag == b"IDAT":
            idat += chunk_data
        elif tag == b"IEND":
            break
    if width is None or height is None:
        raise PngError("missing IHDR")
    raw = zlib.decompress(bytes(idat))
    pixels = _unfilter(raw, width, height)
    return width, height, pixels
