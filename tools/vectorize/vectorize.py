#!/usr/bin/env python3
"""AI-assisted vectorization loop harness for Donner.

Renders a candidate SVG through Donner's own CLI renderer (``donner-svg``),
compares the render against a raster reference image, and emits a machine
readable score plus a visual diff PNG. A coding agent runs this in a loop:

    edit SVG  ->  render (donner-svg)  ->  score  ->  inspect diff PNG  ->  edit

The render oracle is the repository's own renderer, so "does it look right"
is answered by the same code Donner ships, not a third party rasterizer.

Dependencies: Python 3 standard library only (no PIL, no numpy). PNG decode
and encode are implemented in pure Python so the loop runs anywhere python3
runs. Bit depth 8, non-interlaced PNGs are supported (this covers both what
donner-svg emits and the Geode splash reference).

Usage:
    python3 vectorize.py --reference ref.png --svg candidate.svg --out workdir

See AGENT-LOOP.md for the full agent workflow.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Pure-Python PNG codec (no PIL / numpy dependency).
# ---------------------------------------------------------------------------

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Bytes-per-pixel for each PNG color type at bit depth 8.
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


@dataclass
class Image:
    """A decoded RGBA image: width, height, and a flat bytearray of RGBA8."""

    width: int
    height: int
    rgba: bytearray  # length == width * height * 4

    def pixel_index(self, x: int, y: int) -> int:
        return (y * self.width + x) * 4


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(path: str) -> Image:
    """Decode an 8-bit, non-interlaced PNG into an RGBA Image."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != PNG_SIGNATURE:
        raise ValueError(f"{path}: not a PNG (bad signature)")

    pos = 8
    width = height = 0
    bit_depth = color_type = interlace = 0
    idat = bytearray()
    palette: List[Tuple[int, int, int]] = []
    trns: List[int] = []

    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length  # 4 len + 4 type + data + 4 crc

        if ctype == b"IHDR":
            (width, height, bit_depth, color_type, _comp, _filt, interlace) = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"PLTE":
            for i in range(0, len(chunk), 3):
                palette.append((chunk[i], chunk[i + 1], chunk[i + 2]))
        elif ctype == b"tRNS":
            trns = list(chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    if bit_depth != 8:
        raise ValueError(f"{path}: unsupported bit depth {bit_depth} (only 8 supported)")
    if interlace != 0:
        raise ValueError(f"{path}: interlaced PNGs are not supported")
    if color_type not in _CHANNELS:
        raise ValueError(f"{path}: unsupported color type {color_type}")

    channels = _CHANNELS[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels

    # Unfilter scanlines in place.
    recon = bytearray(height * stride)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        filter_type = raw[src]
        src += 1
        line = bytearray(raw[src : src + stride])
        src += stride
        if filter_type == 0:
            pass
        elif filter_type == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        else:
            raise ValueError(f"{path}: bad filter type {filter_type}")
        recon[y * stride : (y + 1) * stride] = line
        prev = line

    # Expand to RGBA.
    rgba = bytearray(width * height * 4)
    for y in range(height):
        row = y * stride
        out = y * width * 4
        for x in range(width):
            si = row + x * channels
            di = out + x * 4
            if color_type == 6:  # RGBA
                rgba[di : di + 4] = recon[si : si + 4]
            elif color_type == 2:  # RGB
                rgba[di : di + 3] = recon[si : si + 3]
                rgba[di + 3] = 255
            elif color_type == 0:  # Gray
                g = recon[si]
                rgba[di] = rgba[di + 1] = rgba[di + 2] = g
                rgba[di + 3] = 255
            elif color_type == 4:  # Gray+Alpha
                g = recon[si]
                rgba[di] = rgba[di + 1] = rgba[di + 2] = g
                rgba[di + 3] = recon[si + 1]
            elif color_type == 3:  # Palette
                idx = recon[si]
                r, g, b = palette[idx]
                rgba[di] = r
                rgba[di + 1] = g
                rgba[di + 2] = b
                rgba[di + 3] = trns[idx] if idx < len(trns) else 255

    return Image(width, height, rgba)


def encode_png(img: Image, path: str) -> None:
    """Encode an RGBA Image to an 8-bit RGBA PNG (filter type 0)."""
    stride = img.width * 4
    raw = bytearray()
    for y in range(img.height):
        raw.append(0)  # filter type None
        raw += img.rgba[y * stride : (y + 1) * stride]

    def chunk(ctype: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + ctype
            + payload
            + struct.pack(">I", zlib.crc32(ctype + payload) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", img.width, img.height, 8, 6, 0, 0, 0)
    out = bytearray(PNG_SIGNATURE)
    out += chunk(b"IHDR", ihdr)
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------
# Image ops: compositing, resizing, diffing.
# ---------------------------------------------------------------------------

_BG_COLORS = {
    "white": (255, 255, 255),
    "black": (0, 0, 0),
    "gray": (128, 128, 128),
}


def flatten_over(img: Image, bg: Tuple[int, int, int]) -> List[Tuple[int, int, int]]:
    """Composite an RGBA image over an opaque background, returning RGB tuples."""
    br, bg_, bb = bg
    out: List[Tuple[int, int, int]] = [(0, 0, 0)] * (img.width * img.height)
    px = img.rgba
    for i in range(img.width * img.height):
        j = i * 4
        a = px[j + 3]
        if a == 255:
            out[i] = (px[j], px[j + 1], px[j + 2])
        elif a == 0:
            out[i] = (br, bg_, bb)
        else:
            inv = 255 - a
            out[i] = (
                (px[j] * a + br * inv) // 255,
                (px[j + 1] * a + bg_ * inv) // 255,
                (px[j + 2] * a + bb * inv) // 255,
            )
    return out


def nearest_resize(img: Image, w: int, h: int) -> Image:
    """Nearest-neighbor resize (fallback only; renders should already match)."""
    if img.width == w and img.height == h:
        return img
    out = bytearray(w * h * 4)
    for y in range(h):
        sy = min(img.height - 1, y * img.height // h)
        for x in range(w):
            sx = min(img.width - 1, x * img.width // w)
            si = (sy * img.width + sx) * 4
            di = (y * w + x) * 4
            out[di : di + 4] = img.rgba[si : si + 4]
    return Image(w, h, out)


@dataclass
class Tile:
    tx: int
    ty: int
    x: int
    y: int
    w: int
    h: int
    rmse: float


@dataclass
class DiffResult:
    width: int
    height: int
    rmse: float  # 0..255
    rmse_normalized: float  # 0..1
    diff_pixel_percent: float  # 0..100
    diff_threshold: int
    quality_score: float  # 0..100, higher is better
    worst_tiles: List[Tile] = field(default_factory=list)
    grid: Tuple[int, int] = (0, 0)
    diff_image: Optional[Image] = None


def diff_images(
    ref: Image,
    cand: Image,
    bg: Tuple[int, int, int],
    threshold: int,
    grid: int,
    worst_k: int,
) -> DiffResult:
    """Compute RMSE, diff-pixel %, per-tile worst regions, and a heatmap."""
    if cand.width != ref.width or cand.height != ref.height:
        cand = nearest_resize(cand, ref.width, ref.height)

    w, h = ref.width, ref.height
    ref_rgb = flatten_over(ref, bg)
    cand_rgb = flatten_over(cand, bg)

    n = w * h
    sq_error_total = 0  # sum of squared channel error
    diff_pixels = 0

    # Heatmap: bright red scaled by per-pixel error magnitude.
    heat = bytearray(n * 4)

    # Per-tile squared-error accumulation.
    gx = max(1, min(grid, w))
    gy = max(1, min(grid, h))
    tile_sq = [[0 for _ in range(gx)] for _ in range(gy)]
    tile_count = [[0 for _ in range(gx)] for _ in range(gy)]

    for y in range(h):
        tyi = y * gy // h
        row = y * w
        for x in range(w):
            i = row + x
            r1, g1, b1 = ref_rgb[i]
            r2, g2, b2 = cand_rgb[i]
            dr = r1 - r2
            dg = g1 - g2
            db = b1 - b2
            sq = dr * dr + dg * dg + db * db
            sq_error_total += sq

            maxch = max(abs(dr), abs(dg), abs(db))
            if maxch > threshold:
                diff_pixels += 1

            txi = x * gx // w
            tile_sq[tyi][txi] += sq
            tile_count[tyi][txi] += 1

            # Heatmap intensity: normalize channel-RMS of this pixel to 0..255.
            mag = int((sq / 3.0) ** 0.5)
            if mag > 255:
                mag = 255
            di = i * 4
            heat[di] = mag  # R
            heat[di + 1] = mag // 4  # a touch of G/B so faint diffs stay visible
            heat[di + 2] = mag // 4
            heat[di + 3] = 255

    rmse = (sq_error_total / (n * 3.0)) ** 0.5
    rmse_norm = rmse / 255.0
    diff_pct = 100.0 * diff_pixels / n
    quality = max(0.0, 100.0 * (1.0 - rmse_norm))

    tiles: List[Tile] = []
    tw = w / gx
    th = h / gy
    for ty in range(gy):
        for tx in range(gx):
            c = tile_count[ty][tx]
            if c == 0:
                continue
            trmse = (tile_sq[ty][tx] / (c * 3.0)) ** 0.5
            x0 = int(tx * tw)
            y0 = int(ty * th)
            x1 = int((tx + 1) * tw)
            y1 = int((ty + 1) * th)
            tiles.append(Tile(tx, ty, x0, y0, x1 - x0, y1 - y0, round(trmse, 3)))
    tiles.sort(key=lambda t: t.rmse, reverse=True)

    return DiffResult(
        width=w,
        height=h,
        rmse=round(rmse, 4),
        rmse_normalized=round(rmse_norm, 6),
        diff_pixel_percent=round(diff_pct, 4),
        diff_threshold=threshold,
        quality_score=round(quality, 4),
        worst_tiles=tiles[:worst_k],
        grid=(gx, gy),
        diff_image=Image(w, h, heat),
    )


# ---------------------------------------------------------------------------
# Renderer oracle: Donner's own donner-svg CLI.
# ---------------------------------------------------------------------------


def find_repo_root(start: str) -> Optional[str]:
    d = os.path.abspath(start)
    while True:
        if os.path.exists(os.path.join(d, "MODULE.bazel")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def default_renderer(repo_root: Optional[str]) -> Optional[str]:
    if not repo_root:
        return None
    cand = os.path.join(repo_root, "bazel-bin", "donner", "svg", "tool", "donner-svg")
    return cand if os.path.exists(cand) else None


def render_svg(renderer: str, svg: str, out_png: str, width: int, height: int) -> None:
    """Render an SVG to PNG at an exact pixel size using donner-svg."""
    cmd = [
        renderer,
        os.path.abspath(svg),
        "--output",
        os.path.abspath(out_png),
        "--width",
        str(width),
        "--height",
        str(height),
        "--quiet",
    ]
    proc = subprocess.run(
        cmd,
        cwd=os.path.dirname(os.path.abspath(svg)) or ".",
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0 or not os.path.exists(out_png):
        raise RuntimeError(
            "donner-svg render failed (exit %d)\ncmd: %s\nstdout:\n%s\nstderr:\n%s"
            % (proc.returncode, " ".join(cmd), proc.stdout, proc.stderr)
        )


# ---------------------------------------------------------------------------
# Main.
# ---------------------------------------------------------------------------


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Donner AI-assisted vectorization loop: render a candidate "
        "SVG through donner-svg, diff it against a raster reference, and emit a score."
    )
    parser.add_argument("--reference", required=True, help="Raster reference PNG.")
    parser.add_argument("--svg", required=True, help="Candidate SVG to score.")
    parser.add_argument("--out", required=True, help="Work directory for outputs.")
    parser.add_argument(
        "--renderer",
        default=None,
        help="Path to donner-svg binary (default: repo bazel-bin build).",
    )
    parser.add_argument(
        "--background",
        default="white",
        choices=sorted(_BG_COLORS.keys()),
        help="Background to composite transparency over before diffing.",
    )
    parser.add_argument(
        "--threshold",
        type=int,
        default=16,
        help="Per-channel abs-diff threshold for counting a pixel as changed.",
    )
    parser.add_argument("--grid", type=int, default=8, help="NxN tile grid for worst-region report.")
    parser.add_argument("--worst", type=int, default=8, help="Number of worst tiles to report.")
    parser.add_argument(
        "--width", type=int, default=0, help="Render width override (default: reference width)."
    )
    parser.add_argument(
        "--height", type=int, default=0, help="Render height override (default: reference height)."
    )
    args = parser.parse_args(argv)

    repo_root = find_repo_root(os.path.dirname(os.path.abspath(__file__)))
    renderer = args.renderer or default_renderer(repo_root)
    if not renderer or not os.path.exists(renderer):
        sys.stderr.write(
            "error: donner-svg renderer not found.\n"
            "Build it first:  bazel build //donner/svg/tool:donner-svg\n"
            "or pass --renderer <path>.\n"
        )
        return 2

    os.makedirs(args.out, exist_ok=True)

    ref = decode_png(args.reference)
    width = args.width or ref.width
    height = args.height or ref.height

    rendered_png = os.path.join(args.out, "rendered.png")
    diff_png = os.path.join(args.out, "diff.png")
    score_json = os.path.join(args.out, "score.json")

    render_svg(renderer, args.svg, rendered_png, width, height)
    cand = decode_png(rendered_png)

    result = diff_images(
        ref,
        cand,
        _BG_COLORS[args.background],
        args.threshold,
        args.grid,
        args.worst,
    )
    encode_png(result.diff_image, diff_png)

    score = {
        "reference": os.path.abspath(args.reference),
        "candidate_svg": os.path.abspath(args.svg),
        "renderer": os.path.abspath(renderer),
        "rendered_png": os.path.abspath(rendered_png),
        "diff_png": os.path.abspath(diff_png),
        "width": result.width,
        "height": result.height,
        "background": args.background,
        "rmse": result.rmse,
        "rmse_normalized": result.rmse_normalized,
        "diff_pixel_percent": result.diff_pixel_percent,
        "diff_threshold": result.diff_threshold,
        "quality_score": result.quality_score,
        "grid": list(result.grid),
        "worst_tiles": [
            {
                "tile": [t.tx, t.ty],
                "x": t.x,
                "y": t.y,
                "w": t.w,
                "h": t.h,
                "rmse": t.rmse,
            }
            for t in result.worst_tiles
        ],
    }
    with open(score_json, "w") as f:
        json.dump(score, f, indent=2)
        f.write("\n")

    print("score.json  ->", score_json)
    print("rendered    ->", rendered_png)
    print("diff PNG    ->", diff_png)
    print(
        "rmse=%.3f  quality=%.2f/100  diff_pixels=%.3f%%  (bg=%s)"
        % (
            result.rmse,
            result.quality_score,
            result.diff_pixel_percent,
            args.background,
        )
    )
    if result.worst_tiles:
        w0 = result.worst_tiles[0]
        print(
            "worst tile [%d,%d] at (%d,%d %dx%d) rmse=%.2f"
            % (w0.tx, w0.ty, w0.x, w0.y, w0.w, w0.h, w0.rmse)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
