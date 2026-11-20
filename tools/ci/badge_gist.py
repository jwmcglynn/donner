"""Renders the README badges and publishes them to their gist in one update.

The gist service rejects a write that arrives while an earlier write to the same gist is still
being applied (HTTP 409), so all badge files go in a single `PATCH /gists/<id>` request.

Each badge is a self-contained flat SVG: a grey label segment and a colored message segment,
sized from a fixed per-character width table for 11px Verdana, so the same values always render
the same bytes. The token is read from the GIST_TOKEN environment variable and is never printed.
A failure exits non-zero after printing a GitHub Actions warning annotation that names it.
The module uses only the Python standard library.

Usage:
  GIST_TOKEN=... badge_gist.py --gist-id ID --badge FILE LABEL MESSAGE COLOR [--badge ...]
"""

from __future__ import annotations

import argparse
import http.client
import json
import math
import os
import re
import sys
import urllib.error
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from typing import Any, TextIO
from xml.sax.saxutils import escape

API_ROOT = "https://api.github.com"
TOKEN_ENV = "GIST_TOKEN"
TIMEOUT_SECONDS = 30
WARNING_TITLE = "Badge update failed"

# The hex values shields-style badges use for these color names.
COLORS = {"lightgrey": "#9f9f9f", "blue": "#007ec6", "green": "#97ca00"}
LABEL_COLOR = "#555"
HEIGHT = 20
PADDING = 10

# Advance widths in tenths of a pixel for 11px Verdana; other characters use DEFAULT_WIDTH.
DEFAULT_WIDTH = 70
_WIDTH_GROUPS = (
    (30, "ilj'"), (39, " .,f"), (43, "t!"), (45, ":;-()[]"), (46, "I"), (47, "r"),
    (50, "J/\\|"), (51, '"'), (57, "s"), (58, "cz"), (59, "?"), (61, "L"), (63, "F"),
    (65, "kvxy"), (66, "eP"), (67, "ao"), (68, "TY"), (69, "bdgpqE"),
    (70, "0123456789hnu$*_`{}"), (75, "ABSVXZ"), (76, "K"), (77, "CR"), (80, "&"),
    (81, "U"), (82, "N"), (83, "H"), (85, "DG"), (87, "OQ"), (90, "w"), (92, "<>+=#~^"),
    (93, "M"), (107, "m"), (109, "W"), (110, "@"), (123, "%"),
)
CHAR_WIDTHS = {char: width for width, chars in _WIDTH_GROUPS for char in chars}

_GIST_ID = re.compile(r"[0-9a-f]{20,64}")
_FILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.svg")

Opener = Callable[[urllib.request.Request, float], Any]


class BadgeError(Exception):
    """A badge or gist request that cannot be built or was refused."""


def text_width(text: str) -> int:
    """The rendered width of `text` in whole pixels, rounded up."""
    return math.ceil(sum(CHAR_WIDTHS.get(char, DEFAULT_WIDTH) for char in text) / 10)


def _checked_text(kind: str, text: str) -> str:
    text = text.strip()
    if not text:
        raise BadgeError(f"badge {kind} is empty")
    if not text.isprintable():
        raise BadgeError(f"badge {kind} contains a non-printable character")
    return text


def _xml(text: str) -> str:
    return escape(text, {'"': "&quot;", "'": "&apos;"})


def _num(value: float) -> str:
    return f"{value:g}"


def _segment_text(text: str, center: float, width: int) -> str:
    escaped = _xml(text)
    return (f'<text aria-hidden="true" x="{_num(center)}" y="15" fill="#010101" '
            f'fill-opacity=".3" textLength="{width}">{escaped}</text>'
            f'<text x="{_num(center)}" y="14" textLength="{width}">{escaped}</text>')


def render_badge(label: str, message: str, color: str) -> str:
    """One flat SVG badge with a grey `label` segment and a `color` `message` segment."""
    if color not in COLORS:
        raise BadgeError(f"unknown badge color {color!r}; expected one of {sorted(COLORS)}")
    label = _checked_text("label", label)
    message = _checked_text("message", message)
    label_text, message_text = text_width(label), text_width(message)
    label_width, message_width = label_text + PADDING, message_text + PADDING
    width = label_width + message_width
    title = _xml(f"{label}: {message}")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{HEIGHT}" '
        f'role="img" aria-label="{title}"><title>{title}</title>'
        '<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" '
        'stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient>'
        f'<clipPath id="r"><rect width="{width}" height="{HEIGHT}" rx="3" fill="#fff"/>'
        '</clipPath><g clip-path="url(#r)">'
        f'<rect width="{label_width}" height="{HEIGHT}" fill="{LABEL_COLOR}"/>'
        f'<rect x="{label_width}" width="{message_width}" height="{HEIGHT}" '
        f'fill="{COLORS[color]}"/>'
        f'<rect width="{width}" height="{HEIGHT}" fill="url(#s)"/></g>'
        '<g fill="#fff" text-anchor="middle" '
        'font-family="Verdana,Geneva,DejaVu Sans,sans-serif" '
        'text-rendering="geometricPrecision" font-size="11">'
        f'{_segment_text(label, label_width / 2, label_text)}'
        f'{_segment_text(message, label_width + message_width / 2, message_text)}'
        '</g></svg>\n')


def render_files(badges: Sequence[Sequence[str]]) -> dict[str, str]:
    """Maps each badge file name to its SVG; `badges` holds (file, label, message, color)."""
    files = {}
    for name, label, message, color in badges:
        if not _FILE_NAME.fullmatch(name):
            raise BadgeError(f"invalid badge file name {name!r}")
        if name in files:
            raise BadgeError(f"duplicate badge file name {name!r}")
        files[name] = render_badge(label, message, color)
    if not files:
        raise BadgeError("no badges to publish")
    return files


def request_body(files: Mapping[str, str]) -> bytes:
    """The gist update body that replaces every file in `files`."""
    return json.dumps({"files": {name: {"content": content} for name, content in files.items()}},
                      separators=(",", ":")).encode("utf-8")


def gist_url(gist_id: str) -> str:
    """The REST URL of one gist; `gist_id` must be a plain hex ID."""
    if not _GIST_ID.fullmatch(gist_id):
        raise BadgeError("invalid gist ID")
    return f"{API_ROOT}/gists/{gist_id}"


def build_request(gist_id: str, token: str, files: Mapping[str, str]) -> urllib.request.Request:
    """The single PATCH request that writes all of `files` to the gist."""
    url = gist_url(gist_id)
    if not (token and token.isascii() and token.isprintable()) or " " in token:
        raise BadgeError(f"{TOKEN_ENV} is missing or malformed")
    return urllib.request.Request(
        url, data=request_body(files), method="PATCH",
        headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                 "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json"})


def _default_opener(request: urllib.request.Request, timeout: float) -> Any:
    return urllib.request.urlopen(request, timeout=timeout)


def send(request: urllib.request.Request, opener: Opener = _default_opener) -> int:
    """Sends `request` once and returns its 2xx status; anything else raises BadgeError."""
    try:
        with opener(request, TIMEOUT_SECONDS) as response:
            status = response.status
    except urllib.error.HTTPError as error:
        error.close()
        raise BadgeError(f"gist update failed: HTTP {error.code} {error.reason}") from None
    except (urllib.error.URLError, http.client.HTTPException, OSError) as error:
        raise BadgeError(f"gist update failed: {type(error).__name__}") from None
    if not 200 <= status < 300:
        raise BadgeError(f"gist update failed: HTTP {status}")
    return status


def warning_annotation(message: str) -> str:
    """A GitHub Actions warning command that shows `message` on the run page."""
    data = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return f"::warning title={WARNING_TITLE}::{data}"


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--gist-id", required=True)
    parser.add_argument("--badge", nargs=4, action="append", default=[],
                        metavar=("FILE", "LABEL", "MESSAGE", "COLOR"))
    parser.add_argument("--dry-run", action="store_true",
                        help="print the request body size and file names without sending")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None, environ: Mapping[str, str] = os.environ,
         opener: Opener = _default_opener, stdout: TextIO = sys.stdout) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    token = environ.get(TOKEN_ENV, "")
    try:
        files = render_files(args.badge)
        if args.dry_run:
            print(f"dry run: PATCH {gist_url(args.gist_id)} with {len(request_body(files))} "
                  f"bytes: {', '.join(files)}", file=stdout)
            return 0
        status = send(build_request(args.gist_id, token, files), opener)
    except BadgeError as error:
        message = str(error).replace(token, "<redacted>") if token else str(error)
        print(warning_annotation(message), file=stdout)
        return 1
    print(f"Updated {', '.join(files)} in gist {args.gist_id}: HTTP {status}", file=stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
