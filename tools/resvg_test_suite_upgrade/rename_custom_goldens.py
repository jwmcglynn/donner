#!/usr/bin/env python3
"""Rename donner/svg/renderer/testdata/golden/resvg-<old>.png files using the
rename map, matching the post-upgrade test names.

Usage:
    rename_custom_goldens.py --goldens-dir donner/svg/renderer/testdata/golden \
        --rename-map tools/resvg_test_suite_upgrade/rename_map.json \
        [--execute]

By default runs in dry-run mode and reports what would be renamed. Pass
--execute to actually perform the `git mv` operations.

Each custom golden is named resvg-<old-stem>.png where <old-stem> matches a
.svg filename from the pre-rename layout (e.g. `resvg-a-fill-010.png` maps
to old filename `a-fill-010.svg`).
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--goldens-dir", type=Path, required=True)
    parser.add_argument("--rename-map", type=Path, required=True)
    parser.add_argument("--execute", action="store_true",
                        help="Actually perform the renames (default: dry-run)")
    args = parser.parse_args()

    rename_data = json.loads(args.rename_map.read_text())
    mapping = rename_data["mapping"]

    golden_files = sorted(args.goldens_dir.glob("resvg-*.png"))
    print(f"Found {len(golden_files)} custom goldens.", file=sys.stderr)

    planned: list[tuple[Path, Path]] = []
    missing: list[Path] = []
    orphaned: list[Path] = []

    for g in golden_files:
        # Extract the old stem: resvg-a-fill-010.png → a-fill-010
        stem = g.stem[len("resvg-"):]
        old_svg = f"svg/{stem}.svg"
        new_rel = mapping.get(old_svg)
        if new_rel is None:
            if old_svg in mapping:
                orphaned.append(g)  # mapped to None (deleted upstream)
            else:
                missing.append(g)   # not in mapping at all
            continue

        # new_rel is "tests/<category>/<feature>/<name>.svg"
        new_stem = Path(new_rel).stem
        new_name = f"resvg-{new_stem}.png"
        new_path = g.with_name(new_name)
        if new_path == g:
            continue  # identity rename
        planned.append((g, new_path))

    print(f"\nPlanned renames: {len(planned)}", file=sys.stderr)
    for old, new in planned:
        print(f"  {old.name} → {new.name}", file=sys.stderr)
    if missing:
        print(f"\nNot in rename map ({len(missing)}):", file=sys.stderr)
        for g in missing:
            print(f"  {g.name}", file=sys.stderr)
    if orphaned:
        print(f"\nOrphaned goldens (test deleted upstream — delete these by hand): {len(orphaned)}",
              file=sys.stderr)
        for g in orphaned:
            print(f"  {g.name}", file=sys.stderr)

    if not args.execute:
        print("\n(dry-run — pass --execute to actually rename)", file=sys.stderr)
        return 0

    for old, new in planned:
        subprocess.run(["git", "mv", str(old), str(new)], check=True)
    print(f"\nRenamed {len(planned)} goldens.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
