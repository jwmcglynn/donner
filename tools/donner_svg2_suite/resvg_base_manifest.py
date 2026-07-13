#!/usr/bin/env python3
"""Generate a corpus-v1 manifest for the vendored resvg-test-suite base layer.

Design 0057 (Milestone 3, "Package the resvg base layer") requires the resvg
base corpus to be described by a *generated* manifest rather than a hand
maintained one: the manifest is a derived fact about the pinned upstream tree,
not new source truth. This tool walks the vendored tree under
``third_party/resvg-test-suite`` (see that directory's ``BUILD.bazel`` for the
pinned commit and MIT license) and emits:

- a corpus-v1 manifest (``manifest.json``) with one test entry per
  ``tests/<category>/<feature>/<name>.svg`` that has a sibling
  ``<name>.png``, plus a sha256 integrity map over every referenced file; and
- optional bundle metadata (``bundle.json``) recording the upstream source
  repository, pinned revision, license, and manifest digest, per the design's
  "Versioning and Distribution" section.

Every imported case gets an empty ``spec_requirements`` list: base import is
unmapped until the requirement audit and oracle-review campaign (design 0057,
"Oracle governance": imported corpus membership is not sufficient
provenance). Upstream bytes are only read here, never modified, moved, or
copied.

Generation is deterministic: tests are sorted by id, the integrity map is
sorted by path (``json.dumps(..., sort_keys=True)`` handles this), and the
manifest is serialized with sorted keys and fixed indentation, so running this
tool twice against an unchanged tree produces byte-identical output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any

CORPUS_NAME = "resvg"
CORPUS_SCHEMA_URL = "https://donner.graphics/svg2-suite/corpus-v1.schema.json"
DEFAULT_RESVG_ROOT = Path("third_party/resvg-test-suite")

# Pinned upstream commit documented in third_party/resvg-test-suite/BUILD.bazel.
# detect_revision() parses that file for the authoritative value; this is only
# the fallback used if the header cannot be found or parsed.
DEFAULT_REVISION = "d8e064337faf01bc5a9579187a56dbdbe3eacc72"
UPSTREAM_REPOSITORY = "https://github.com/linebender/resvg-test-suite"
UPSTREAM_LICENSE_ID = "MIT"
UPSTREAM_LICENSE_FILE = "LICENSE"

_REVISION_RE = re.compile(r"\bcommit\s+([0-9a-f]{40})\b")
_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Top-level tests/ category -> derived capability tokens. Anything not listed
# here gets no capability tokens; this is a minimal, honest derivation, not a
# full capability taxonomy.
_CATEGORY_CAPABILITIES = {
    "filters": ["filter-effects"],
    "text": ["text"],
}


class ManifestGenerationError(ValueError):
    """Raised when the vendored tree cannot be walked into a valid manifest."""


def read_png_dimensions(path: Path) -> tuple[int, int]:
    """Read ``(width, height)`` from a PNG file's IHDR chunk.

    This only inspects the fixed 8-byte signature and the first chunk header
    plus the leading width/height fields of IHDR; it never decodes pixel
    data, so it has no dependency on the suite's PNG codec and does not
    assume any particular color type or bit depth.
    """

    with open(path, "rb") as handle:
        header = handle.read(8 + 8 + 8)  # signature + chunk length/tag + IHDR width/height
    if len(header) < 24 or header[:8] != _PNG_SIGNATURE:
        raise ManifestGenerationError(f"not a PNG file: {path}")
    tag = header[12:16]
    if tag != b"IHDR":
        raise ManifestGenerationError(f"expected IHDR as the first chunk: {path}")
    width, height = struct.unpack(">II", header[16:24])
    return width, height


def detect_revision(resvg_root: Path, override: str | None) -> str:
    """Return the pinned upstream revision, preferring the BUILD.bazel header."""

    if override:
        return override
    build_file = resvg_root / "BUILD.bazel"
    if build_file.is_file():
        text = build_file.read_text(encoding="utf-8")
        match = _REVISION_RE.search(text)
        if match:
            return match.group(1)
    return DEFAULT_REVISION


def _capabilities_for_category(category: str) -> list[str]:
    return list(_CATEGORY_CAPABILITIES.get(category, []))


def discover_cases(resvg_root: Path) -> list[tuple[Path, Path]]:
    """Return every ``(svg_path, png_path)`` pair with both files present.

    Sorted by the svg path's corpus-relative form so downstream iteration is
    deterministic even before the final id-sort of the manifest's test list.
    """

    tests_root = resvg_root / "tests"
    if not tests_root.is_dir():
        raise ManifestGenerationError(f"no tests/ directory under resvg root: {resvg_root}")

    cases: list[tuple[Path, Path]] = []
    for svg_path in tests_root.rglob("*.svg"):
        png_path = svg_path.with_suffix(".png")
        if png_path.is_file():
            cases.append((svg_path, png_path))

    cases.sort(key=lambda pair: pair[0].relative_to(resvg_root).as_posix())
    return cases


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def build_manifest(resvg_root: Path, revision: str | None = None) -> dict[str, Any]:
    """Walk ``resvg_root`` and build a corpus-v1 manifest dict for it."""

    resolved_revision = detect_revision(resvg_root, revision)
    cases = discover_cases(resvg_root)

    tests: list[dict[str, Any]] = []
    integrity: dict[str, str] = {}

    for svg_path, png_path in cases:
        svg_relative = svg_path.relative_to(resvg_root).as_posix()
        png_relative = png_path.relative_to(resvg_root).as_posix()
        relative_to_tests = svg_path.relative_to(resvg_root / "tests")
        test_id_suffix = relative_to_tests.with_suffix("").as_posix()
        category = relative_to_tests.parts[0]
        width, height = read_png_dimensions(png_path)

        tests.append(
            {
                "id": f"{CORPUS_NAME}/{test_id_suffix}",
                "input": svg_relative,
                "oracle": {
                    "kind": "png",
                    "path": png_relative,
                    "width": width,
                    "height": height,
                    "provenance": "upstream-resvg-golden",
                },
                "assertion": (
                    f"Imported resvg base case {relative_to_tests.as_posix()}; "
                    "upstream reference rendering (unreviewed for SVG 2 "
                    "requirement mapping)."
                ),
                "spec_requirements": [],
                "capabilities": _capabilities_for_category(category),
            }
        )
        integrity[svg_relative] = _hash_file(svg_path)
        integrity[png_relative] = _hash_file(png_path)

    tests.sort(key=lambda test: test["id"])

    return {
        "schema": CORPUS_SCHEMA_URL,
        "corpus": CORPUS_NAME,
        "revision": resolved_revision,
        "integrity": integrity,
        "tests": tests,
    }


def render_manifest(manifest: dict[str, Any]) -> str:
    """Serialize ``manifest`` deterministically, with a trailing newline."""

    return json.dumps(manifest, indent=2, sort_keys=True) + "\n"


def build_bundle(manifest: dict[str, Any], manifest_text: str) -> dict[str, Any]:
    """Build the bundle metadata dict (design "Versioning and Distribution")."""

    manifest_sha256 = hashlib.sha256(manifest_text.encode("utf-8")).hexdigest()
    return {
        "corpus": manifest["corpus"],
        "source": {
            "repository": UPSTREAM_REPOSITORY,
            "revision": manifest["revision"],
        },
        "license": {
            "id": UPSTREAM_LICENSE_ID,
            "file": UPSTREAM_LICENSE_FILE,
        },
        "schema": "corpus-v1",
        "manifest_sha256": f"sha256:{manifest_sha256}",
        "test_count": len(manifest["tests"]),
    }


def render_bundle(bundle: dict[str, Any]) -> str:
    return json.dumps(bundle, indent=2, sort_keys=True) + "\n"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a corpus-v1 manifest for the vendored resvg-test-suite."
    )
    parser.add_argument(
        "--resvg-root",
        type=Path,
        default=DEFAULT_RESVG_ROOT,
        help="Root of the vendored resvg-test-suite tree (default: third_party/resvg-test-suite)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Write the manifest JSON here (default: stdout)",
    )
    parser.add_argument(
        "--revision",
        type=str,
        default=None,
        help="Override the detected upstream revision instead of parsing BUILD.bazel",
    )
    parser.add_argument(
        "--bundle-out",
        type=Path,
        default=None,
        help="Also write bundle.json metadata (source, license, manifest digest) here",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    manifest = build_manifest(args.resvg_root, args.revision)
    manifest_text = render_manifest(manifest)

    if args.out is not None:
        args.out.write_text(manifest_text, encoding="utf-8")
    else:
        sys.stdout.write(manifest_text)

    if args.bundle_out is not None:
        bundle = build_bundle(manifest, manifest_text)
        args.bundle_out.write_text(render_bundle(bundle), encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
