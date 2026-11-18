"""Compute the 500px reference control for feSpotLight/complex-transform.svg.

The model reads no image. It evaluates the fixture's radial source height and the
selected device-space lighting convention, then adds the authored black frame.
"""

import argparse
from functools import lru_cache
import math
from pathlib import Path
import struct
import zlib


SIZE = 500
# viewBox scale 2.5 composed with skewX(30) and rotate(30).
XX = 5.0 / math.sqrt(3.0)
YX = 1.25
YY = 1.25 * math.sqrt(3.0)
DEPTH_SCALE = math.hypot(math.hypot(XX, YX), YY) / math.sqrt(2.0)
CONE_CUTOFF = math.cos(math.radians(20.0))
CONE_FADE = 0.016
LIGHT_X, LIGHT_Y, LIGHT_Z = 140.0, 150.0, 40.0


def byte(value: float) -> int:
    return max(0, min(255, math.floor(value * 255 + 0.5)))


def user_position(x: float, y: float) -> tuple[float, float]:
    u = x / XX
    return u, (y - YX * u) / YY


def unit(x: float, y: float, z: float) -> tuple[float, float, float]:
    length = math.hypot(x, y, z)
    return (x / length, y / length, z / length) if length else (0.0, 0.0, 0.0)


def dot(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    return sum(x * y for x, y in zip(a, b))


@lru_cache(maxsize=8192)
def source_height(x: int, y: int) -> float:
    u, v = user_position(x + 0.5, y + 0.5)
    radius = math.hypot(u - 80.0, v - 70.0)
    # Alpha is opaque to radius 25, then falls linearly to zero at radius 50.
    return byte((50.0 - radius) / 25.0) / 255.0


def surface_normal(x: int, y: int) -> tuple[float, float, float]:
    weights = ((-1, 1), (0, 2), (1, 1))
    dx = sum(w * (source_height(x + 1, y + i) - source_height(x - 1, y + i))
             for i, w in weights)
    dy = sum(w * (source_height(x + i, y + 1) - source_height(x + i, y - 1))
             for i, w in weights)
    return unit(-dx / 4.0, -dy / 4.0, 1.0)


def intensity(x: int, y: int, hard_cone: bool) -> int:
    height = source_height(x, y)
    rx, ry = XX * LIGHT_X, YX * LIGHT_X + YY * LIGHT_Y
    rz = LIGHT_Z * DEPTH_SCALE
    light = unit(rx - x, ry - y, rz - height)
    if dot(light, unit(rx, ry, rz)) <= 0.0:
        return 0
    lambert = max(0.0, dot(surface_normal(x, y), light))
    # Cone axis points from the authored light toward the default target (0,0,0).
    u, v = user_position(x, y)
    direction = unit(u - LIGHT_X, v - LIGHT_Y, height - LIGHT_Z)
    cosine = dot(direction, unit(-LIGHT_X, -LIGHT_Y, -LIGHT_Z))
    fade = min(1.0, max(0.0, (cosine - CONE_CUTOFF) / CONE_FADE))
    if hard_cone:
        fade = float(cosine >= CONE_CUTOFF)
    return byte(lambert * max(0.0, cosine) * fade)


def overlap(pixel: int, start: float, end: float) -> float:
    return max(0.0, min(pixel + 1.0, end) - max(pixel, start))


def rgba(x: int, y: int, hard_cone: bool) -> bytes:
    u, v = user_position(x + 0.5, y + 0.5)
    # The 100×100 source box plus the default 10% filter margins.
    if 20 <= u < 140 and 10 <= v < 130:
        value = intensity(x, y, hard_cone)
        return bytes((value, value, value, 255))
    # The stroke of the 198-unit frame is disjoint from the filtered area.
    outer = overlap(x, 1.25, 498.75) * overlap(y, 1.25, 498.75)
    inner = overlap(x, 3.75, 496.25) * overlap(y, 3.75, 496.25)
    return bytes((0, 0, 0, byte(outer - inner)))


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload)))


def write_png(path: Path, hard_cone: bool) -> None:
    scanlines = b"".join(b"\x00" + b"".join(rgba(x, y, hard_cone) for x in range(SIZE))
                         for y in range(SIZE))
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hard-cone", action="store_true",
                        help="omit the cutoff fade for a negative control")
    args = parser.parse_args()
    write_png(args.output, args.hard_cone)
