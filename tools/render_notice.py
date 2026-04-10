"""Render an aggregated NOTICE.txt from a Donner licenses manifest.

Invoked by the `donner_notice_file` build rule. Reads the JSON manifest that
lists each dependency's metadata, resolves each entry's license text file via
the `--license-text SHORT_PATH=ACTUAL_PATH` arguments (which the rule passes
so we can find files inside the action sandbox), and concatenates a
human-readable NOTICE suitable for embedding in an application.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", help="Input manifest JSON path")
    parser.add_argument("output", help="Output NOTICE.txt path")
    parser.add_argument(
        "--license-text",
        action="append",
        default=[],
        metavar="SHORT_PATH=ACTUAL_PATH",
        help="Mapping from manifest short_path to sandbox path (repeatable).",
    )
    return parser.parse_args(argv)


def _build_lookup(pairs: list[str]) -> dict[str, Path]:
    lookup: dict[str, Path] = {}
    for pair in pairs:
        short, _, actual = pair.partition("=")
        if not actual:
            raise ValueError(f"Invalid --license-text value: {pair!r}")
        lookup[short] = Path(actual)
    return lookup


def render(manifest_path: Path, output_path: Path, lookup: dict[str, Path]) -> None:
    manifest = json.loads(manifest_path.read_text())
    entries = sorted(
        manifest["licenses"],
        key=lambda entry: (entry.get("package_name") or entry["label"]).lower(),
    )

    lines: list[str] = [
        f"Third-party licenses for build variant: {manifest['variant']}",
        "=" * 72,
        "",
        "This file lists third-party components included in this build and",
        "the license under which each is distributed. Retain this attribution",
        "when redistributing binaries built from this codebase.",
        "",
    ]

    for entry in entries:
        lines.append("-" * 72)
        lines.append(f"Package: {entry.get('package_name') or entry['label']}")
        if entry.get("package_version"):
            lines.append(f"Version: {entry['package_version']}")
        if entry.get("package_url"):
            lines.append(f"URL: {entry['package_url']}")
        kinds = ", ".join(entry.get("license_kinds") or [])
        if kinds:
            lines.append(f"License: {kinds}")
        if entry.get("copyright_notice"):
            lines.append(f"Copyright: {entry['copyright_notice']}")
        lines.append("")

        short = entry["license_text"]
        sandbox_path = lookup.get(short, Path(short))
        try:
            text = sandbox_path.read_text(errors="replace").rstrip()
        except OSError as exc:
            raise SystemExit(
                f"render_notice: failed to read license text {sandbox_path} "
                f"(manifest short_path={short!r}): {exc}"
            )
        lines.append(text)
        lines.append("")

    output_path.write_text("\n".join(lines))


def main(argv: list[str]) -> int:
    args = _parse_args(argv)
    lookup = _build_lookup(args.license_text)
    render(Path(args.manifest), Path(args.output), lookup)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
