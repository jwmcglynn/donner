#!/usr/bin/env python3
"""Convert ANSI truecolor terminal output into a terminal-screenshot SVG.

donner-svg's ``--preview`` mode renders an SVG to the terminal using Unicode
block/quadrant characters with 24-bit foreground/background colors. Each
character cell is a 2×2 sub-pixel grid (upper-left, upper-right, lower-left,
lower-right). A full character cell is thus 2 columns × 2 rows of sub-pixels,
and each sub-pixel is coloured with either the current foreground or the
current background depending on which glyph is used.

Supported characters (U+2580…U+259F block elements):

    ▀ U+2580  UL+UR fg, LL+LR bg           █ U+2588  all fg
    ▄ U+2584  LL+LR fg, UL+UR bg           ▙ U+2599  UL+LL+LR fg, UR bg
    ▌ U+258C  UL+LL fg, UR+LR bg           ▚ U+259A  UL+LR fg, UR+LL bg
    ▐ U+2590  UR+LR fg, UL+LL bg           ▛ U+259B  UL+UR+LL fg, LR bg
    ▖ U+2596  LL fg, rest bg               ▜ U+259C  UL+UR+LR fg, LL bg
    ▗ U+2597  LR fg, rest bg               ▝ U+259D  UR fg, rest bg
    ▘ U+2598  UL fg, rest bg               ▞ U+259E  UR+LL fg, UL+LR bg
                                           ▟ U+259F  UR+LL+LR fg, UL bg

The output SVG is styled as a terminal screenshot: a dark background, a
prompt line showing the command that was run, and the preview grid rendered
as per-sub-pixel ``<rect>`` elements with run-length encoding along rows.

Usage:
    ./tools/ansi_terminal_to_svg.py INPUT.ans OUTPUT.svg [--prompt "$ cmd"]
    cat terminal.ans | ./tools/ansi_terminal_to_svg.py - OUTPUT.svg --prompt "$ cmd"
"""

from __future__ import annotations

import argparse
import re
import sys

_ANSI_COLOR = re.compile(r"\x1b\[(3|4)8;2;(\d+);(\d+);(\d+)m")
_ANSI_RESET = re.compile(r"\x1b\[0m")
_ANSI_OTHER = re.compile(r"\x1b\[[?\d;]*[A-Za-z]")

# Sub-pixel mask per glyph. Bits are UL(1) UR(2) LL(4) LR(8).
# 0 = sub-pixel uses bg color, 1 = sub-pixel uses fg color.
_GLYPH_MASK: dict[str, int] = {
    "\u2580": 0b0011,  # ▀  UL+UR fg
    "\u2584": 0b1100,  # ▄  LL+LR fg
    "\u2588": 0b1111,  # █  all fg
    "\u258c": 0b0101,  # ▌  UL+LL fg
    "\u2590": 0b1010,  # ▐  UR+LR fg
    "\u2596": 0b0100,  # ▖  LL fg
    "\u2597": 0b1000,  # ▗  LR fg
    "\u2598": 0b0001,  # ▘  UL fg
    "\u2599": 0b1101,  # ▙  UL+LL+LR fg
    "\u259a": 0b1001,  # ▚  UL+LR fg
    "\u259b": 0b0111,  # ▛  UL+UR+LL fg
    "\u259c": 0b1011,  # ▜  UL+UR+LR fg
    "\u259d": 0b0010,  # ▝  UR fg
    "\u259e": 0b0110,  # ▞  UR+LL fg
    "\u259f": 0b1110,  # ▟  UR+LL+LR fg
    " ": 0b0000,
}

Color = tuple[int, int, int]


def parse_ansi(text: str) -> list[list[tuple[Color, Color, int]]]:
    """Parse ANSI text into a grid of (fg, bg, mask) triples per character cell.

    Returns a list of rows, each row a list of (fg, bg, mask) triples. ``mask``
    is a 4-bit number encoding which sub-pixels should be drawn with the fg
    color (bits: UL=1, UR=2, LL=4, LR=8).
    """
    rows: list[list[tuple[Color, Color, int]]] = []
    current: list[tuple[Color, Color, int]] = []
    fg: Color = (255, 255, 255)
    bg: Color = (0, 0, 0)

    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\n":
            rows.append(current)
            current = []
            i += 1
            continue

        m = _ANSI_COLOR.match(text, i)
        if m:
            r, g, b = int(m.group(2)), int(m.group(3)), int(m.group(4))
            if m.group(1) == "3":
                fg = (r, g, b)
            else:
                bg = (r, g, b)
            i = m.end()
            continue

        m = _ANSI_RESET.match(text, i)
        if m:
            fg = (255, 255, 255)
            bg = (0, 0, 0)
            i = m.end()
            continue

        m = _ANSI_OTHER.match(text, i)
        if m:
            i = m.end()
            continue

        if ch in _GLYPH_MASK:
            current.append((fg, bg, _GLYPH_MASK[ch]))
            i += 1
            continue

        # Unknown character: treat as blank cell so rows stay aligned.
        if ch.isprintable() or ch == "\r" or ch == "\t":
            current.append((bg, bg, 0))
        i += 1

    if current:
        rows.append(current)
    return rows


def _subpixel_color(cell: tuple[Color, Color, int], quadrant: int) -> Color:
    fg, bg, mask = cell
    return fg if (mask >> quadrant) & 1 else bg


def _rect(x: int, y: int, w: int, h: int, color: Color) -> str:
    r, g, b = color
    return (
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" '
        f'fill="#{r:02x}{g:02x}{b:02x}" />'
    )


def render_svg(
    rows: list[list[tuple[Color, Color, int]]],
    prompt: str,
    char_w: int = 12,
    char_h: int = 24,
) -> str:
    """Render the parsed grid into a terminal-screenshot SVG.

    Each character cell is ``char_w × char_h`` pixels (defaults to 12×24, the
    standard ~1:2 terminal cell aspect), so each 2×2 sub-pixel within a cell
    is ``char_w/2 × char_h/2``. This matches how the preview actually looks
    in a terminal — the grid has the same overall aspect as the source SVG.

    A prompt line is drawn above the grid in a monospace font. No window
    chrome is drawn; the output is just a dark background + prompt + grid.
    """
    if not rows:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0" />'

    if char_w % 2 or char_h % 2:
        raise ValueError("char_w and char_h must be even")

    cols = max(len(row) for row in rows)
    subpx_w = char_w // 2
    subpx_h = char_h // 2
    grid_w = cols * char_w
    grid_h = len(rows) * char_h

    padding = 20
    prompt_h = char_h  # one terminal row tall
    prompt_gap = 8
    tail_h = char_h  # trailing "$" prompt line after output
    corner_r = 10
    total_w = grid_w + 2 * padding
    total_h = padding + prompt_h + prompt_gap + grid_h + prompt_gap + tail_h + padding

    out: list[str] = []
    out.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {total_w} {total_h}" '
        f'width="{total_w}" height="{total_h}" '
        f'shape-rendering="crispEdges" '
        f'font-family="Menlo, Consolas, monospace" font-size="{int(char_h * 0.75)}">'
    )

    # The rounded terminal rect is drawn with a dark fill. Outside the
    # rounded corners the SVG viewport is transparent, which would bleed the
    # host page's background through as visible "triangles" on the corners.
    # We paint the viewport behind the rounded rect with a backdrop that
    # matches typical page backgrounds (white for light mode, near-black for
    # dark mode). The CSS media query makes it adapt to the user's OS/browser
    # color scheme so the corners blend with the host page.
    out.append(
        '<style>'
        '.term-backdrop { fill: #ffffff; }'
        '@media (prefers-color-scheme: dark) {'
        '.term-backdrop { fill: #1a1a1a; }'
        '}'
        '</style>'
    )
    out.append(
        f'<rect class="term-backdrop" x="0" y="0" '
        f'width="{total_w}" height="{total_h}" />'
    )

    # Rounded terminal background with a thin border.
    out.append(
        f'<rect x="0.5" y="0.5" width="{total_w - 1}" height="{total_h - 1}" '
        f'rx="{corner_r}" ry="{corner_r}" fill="#1e1e1e" '
        f'stroke="#3a3a3a" stroke-width="1" />'
    )

    # Prompt line. Render the `$ ` prefix dim and the command white, with a
    # proper single-space gap between them by measuring a two-char offset.
    prompt_y = padding + int(prompt_h * 0.78)
    prompt_x = padding
    if prompt.startswith("$ "):
        out.append(
            f'<text x="{prompt_x}" y="{prompt_y}" fill="#27c93f">$</text>'
            f'<text x="{prompt_x + 2 * char_w}" y="{prompt_y}" fill="#d4d4d4">'
            f'{_xml_escape(prompt[2:])}</text>'
        )
    else:
        out.append(
            f'<text x="{prompt_x}" y="{prompt_y}" fill="#d4d4d4">{_xml_escape(prompt)}</text>'
        )

    grid_ox = padding
    grid_oy = padding + prompt_h + prompt_gap
    # Black backdrop behind the preview (cells that hit (0,0,0) can merge with it).
    out.append(_rect(grid_ox, grid_oy, grid_w, grid_h, (0, 0, 0)))

    for row_idx, row in enumerate(rows):
        for subrow in (0, 1):  # 0 = top (UL/UR), 1 = bottom (LL/LR)
            y = grid_oy + row_idx * char_h + subrow * subpx_h
            x_start = 0
            current_color: Color | None = None
            for col_idx, cell in enumerate(row):
                for subcol in (0, 1):  # 0 = left (UL/LL), 1 = right (UR/LR)
                    quadrant = subrow * 2 + subcol
                    color = _subpixel_color(cell, quadrant)
                    pixel_x = col_idx * 2 + subcol  # in sub-pixel units
                    if color != current_color:
                        if current_color is not None:
                            run_w = (pixel_x - x_start) * subpx_w
                            out.append(
                                _rect(
                                    grid_ox + x_start * subpx_w,
                                    y,
                                    run_w,
                                    subpx_h,
                                    current_color,
                                )
                            )
                        current_color = color
                        x_start = pixel_x
            if current_color is not None:
                # Flush trailing run using *this row's* width, not the global
                # max `cols` — otherwise shorter rows smear their last color
                # across the missing trailing cells.
                run_w = (len(row) * 2 - x_start) * subpx_w
                out.append(
                    _rect(
                        grid_ox + x_start * subpx_w,
                        y,
                        run_w,
                        subpx_h,
                        current_color,
                    )
                )

    # Trailing prompt line after the command output. The block cursor is
    # drawn as a "█" text glyph in the same font as the "$" so it inherits
    # the font's metrics exactly and stays aligned regardless of char_w/h.
    tail_y = grid_oy + grid_h + prompt_gap + int(tail_h * 0.78)
    cursor_x = padding + 2 * char_w
    out.append(
        f'<text x="{padding}" y="{tail_y}" fill="#27c93f">$</text>'
        f'<text x="{cursor_x}" y="{tail_y}" fill="#d4d4d4">\u2588</text>'
    )

    out.append("</svg>")
    return "".join(out)


def _xml_escape(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", help="Path to an ANSI text file, or '-' for stdin")
    parser.add_argument("output", help="Path to the output SVG file")
    parser.add_argument(
        "--prompt",
        default="$ tools/donner-svg donner_icon.svg --preview",
        help="Prompt line to display above the preview grid",
    )
    parser.add_argument(
        "--char-width",
        type=int,
        default=12,
        help="Width of each terminal character cell in pixels (default: 12)",
    )
    parser.add_argument(
        "--char-height",
        type=int,
        default=24,
        help="Height of each terminal character cell in pixels (default: 24)",
    )
    args = parser.parse_args()

    if args.input == "-":
        text = sys.stdin.buffer.read().decode("utf-8", errors="replace")
    else:
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()

    # Strip donner-svg's "Rendered size: WxH" banner so it doesn't pollute
    # the preview grid.
    text = re.sub(r"^Rendered size: \d+x\d+\n", "", text)

    rows = parse_ansi(text)
    svg = render_svg(
        rows,
        prompt=args.prompt,
        char_w=args.char_width,
        char_h=args.char_height,
    )

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(svg)
    print(
        f"Wrote {args.output} ({len(svg)} bytes, "
        f"{len(rows)} rows × {max((len(r) for r in rows), default=0)} cols)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
