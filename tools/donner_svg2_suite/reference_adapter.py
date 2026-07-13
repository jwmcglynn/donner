#!/usr/bin/env python3
"""A deterministic reference render adapter for the SVG2 suite runner tests.

This is not a real SVG renderer and, like any conforming adapter, it is a
self-contained executable that depends only on the standard library and the
request/response contract, not on the suite's own modules. It "renders" by
copying its (already-rasterized) PNG input to the output, so a test controls
pass versus comparison-fail purely by choosing the oracle pixels. Behavioural
branches are keyed off marker substrings in the input filename so tests can
force each status path:

- ``unsupported`` (or an ``unsupported`` capability): report an unsupported skip;
- ``crash``: exit non-zero without a response (drives adapter-error);
- ``hang``: sleep far past any reasonable timeout (drives timeout);
- ``malformed``: write a non-JSON response (drives adapter-error);
- ``reperror``: report a structured adapter error; and
- otherwise: copy the input PNG to the output and report its dimensions.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import time


_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _png_dimensions(path: str) -> tuple[int, int]:
    with open(path, "rb") as handle:
        header = handle.read(24)
    if header[:8] != _PNG_SIGNATURE or header[12:16] != b"IHDR":
        raise ValueError("input is not a PNG")
    width, height = struct.unpack(">II", header[16:24])
    return width, height


def _write_response(path: str, status: str, **fields: object) -> None:
    document: dict[str, object] = {"status": status}
    document.update(fields)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)


def _run(request_path: str, response_path: str) -> int:
    with open(request_path, "r", encoding="utf-8") as handle:
        request = json.load(handle)

    input_path = request["input"]
    output_path = request["output"]
    capabilities = request.get("capabilities", [])
    marker = os.path.basename(input_path)

    if "unsupported" in capabilities or "unsupported" in marker:
        _write_response(response_path, "unsupported", diagnostics="declared capability absent")
        return 0
    if "crash" in marker:
        sys.stderr.write("simulated adapter crash\n")
        return 3
    if "hang" in marker:
        time.sleep(60)
        return 0
    if "malformed" in marker:
        with open(response_path, "w", encoding="utf-8") as handle:
            handle.write("{ this is not valid json")
        return 0
    if "reperror" in marker:
        _write_response(response_path, "error", diagnostics="simulated internal failure")
        return 0

    try:
        width, height = _png_dimensions(input_path)
    except (OSError, ValueError) as error:
        _write_response(response_path, "error", diagnostics=f"cannot read input: {error}")
        return 0

    shutil.copyfile(input_path, output_path)
    _write_response(response_path, "ok", width=width, height=height, format="rgba8")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Reference SVG2 render adapter (test fixture).")
    parser.add_argument("operation", choices=["render"])
    parser.add_argument("--request", required=True)
    parser.add_argument("--response", required=True)
    args = parser.parse_args(argv)
    return _run(args.request, args.response)


if __name__ == "__main__":
    raise SystemExit(main())
