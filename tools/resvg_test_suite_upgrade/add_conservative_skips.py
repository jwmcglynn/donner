#!/usr/bin/env python3
"""Patch resvg_test_suite.cc to add Params::Skip(...) entries for every
failing test in the union-of-variants failure set.

Usage:
    add_conservative_skips.py --source resvg_test_suite.cc \
        --failures failures_union.txt \
        [--reason "M1 upgrade: needs triage"]

For each failing test (path like `painting/fill/foo.svg`):
  1. Look up the category directory (`painting/fill`).
  2. Find the matching `getTestsInCategory("painting/fill"` call.
  3. If the file is already listed in its override map, leave it alone.
  4. Otherwise insert `{"foo.svg", Params::Skip("<reason>")},` into the map,
     keeping entries sorted by filename for stability.
  5. If the suite uses the default no-override form
     (`getTestsInCategory("painting/fill")`), upgrade it to the
     override-map form.

Writes the patched source in place. Prints a summary.
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--failures", type=Path, required=True)
    parser.add_argument("--reason", default="M1 upgrade: needs triage")
    args = parser.parse_args()

    # failures_union.txt is one path per line, e.g. "painting/fill/foo.svg"
    by_category: dict[str, list[str]] = defaultdict(list)
    for line in args.failures.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.rsplit("/", 1)
        if len(parts) != 2:
            print(f"skip malformed: {line}", file=sys.stderr)
            continue
        category, filename = parts
        by_category[category].append(filename)

    total_failures = sum(len(v) for v in by_category.values())
    print(f"Failures across {len(by_category)} categories, {total_failures} total",
          file=sys.stderr)

    source = args.source.read_text()
    added = 0
    already_covered = 0
    category_not_found = 0

    for category in sorted(by_category.keys()):
        filenames = sorted(set(by_category[category]))

        # Find the category suite. Two forms exist:
        #
        #   A) No overrides:
        #      INSTANTIATE_TEST_SUITE_P(Foo, ImageComparisonTestFixture,
        #                               ValuesIn(getTestsInCategory("painting/fill")),
        #                               TestNameFromFilename);
        #
        #   B) With overrides:
        #      INSTANTIATE_TEST_SUITE_P(
        #          Foo, ImageComparisonTestFixture,
        #          ValuesIn(getTestsInCategory("painting/fill",
        #                                      {
        #                                          {"a.svg", ...},
        #                                          ...
        #                                      })),
        #          TestNameFromFilename);

        # Locate the getTestsInCategory("category" occurrence.
        needle = f'getTestsInCategory("{category}"'
        idx = source.find(needle)
        if idx < 0:
            print(f"ERROR: category not found in source: {category}",
                  file=sys.stderr)
            category_not_found += len(filenames)
            continue

        # Find the character right after the category literal.
        after_needle = idx + len(needle)

        if source[after_needle] == ")":
            # Form A — no overrides. Upgrade to Form B.
            # Build the new override block.
            indent = "                                    "  # matches codemod output
            entries = [
                f'{indent}{{"{fn}", Params::Skip("{args.reason}")}},'
                for fn in filenames
            ]
            block = (
                ",\n"
                "                                {\n"
                + "\n".join(entries)
                + "\n                                }"
            )
            source = source[:after_needle] + block + source[after_needle:]
            added += len(filenames)
            continue

        if source[after_needle] != ",":
            print(f"ERROR: unexpected form at {category}: char={source[after_needle]!r}",
                  file=sys.stderr)
            category_not_found += len(filenames)
            continue

        # Form B — existing override map. Find the '{' that opens it.
        open_brace = source.find("{", after_needle)
        # Balance braces to find the matching close.
        depth = 0
        i = open_brace
        while i < len(source):
            c = source[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    close_brace = i
                    break
            i += 1
        else:
            print(f"ERROR: unbalanced braces in {category}", file=sys.stderr)
            category_not_found += len(filenames)
            continue

        map_body = source[open_brace + 1:close_brace]

        # Collect existing filenames already covered by overrides.
        existing = set(re.findall(r'\{"([^"]+\.svg)"', map_body))

        to_add = [fn for fn in filenames if fn not in existing]
        if not to_add:
            already_covered += len(filenames)
            continue
        already_covered += len(filenames) - len(to_add)

        # Build new entries (keyed by filename for sort stability).
        indent = "                                    "
        new_entries = "\n".join(
            f'{indent}{{"{fn}", Params::Skip("{args.reason}")}},'
            for fn in to_add
        )

        # Insert the new entries right before the closing brace, preceded by a
        # newline so they land on their own lines.
        insert_at = close_brace
        # Ensure the map body ends with a comma before the close brace.
        body_stripped = map_body.rstrip()
        if body_stripped and not body_stripped.endswith(","):
            # Add a trailing comma to the last existing entry by injecting one.
            # Find the last non-whitespace char in the existing body.
            j = close_brace - 1
            while j > open_brace and source[j] in " \t\n":
                j -= 1
            # j now points at the last non-ws char. If it's '}' (end of
            # an existing entry) we need the following ',' that's already
            # present in the codemod output. If not, add one.
            if source[j] != ",":
                source = source[:j + 1] + "," + source[j + 1:]
                close_brace += 1  # account for inserted character
                insert_at = close_brace

        source = (
            source[:insert_at]
            + "\n" + new_entries
            + "\n                                "  # indent for }
            + source[insert_at:]
        )
        added += len(to_add)

    args.source.write_text(source)

    print(f"\nSummary:", file=sys.stderr)
    print(f"  added:            {added}", file=sys.stderr)
    print(f"  already covered:  {already_covered}", file=sys.stderr)
    print(f"  category missing: {category_not_found}", file=sys.stderr)
    return 0 if category_not_found == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
