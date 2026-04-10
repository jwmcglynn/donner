#!/usr/bin/env python3
"""Replace `Params::Skip("M1 upgrade: needs triage")` entries inside one
INSTANTIATE_TEST_SUITE_P / getTestsInCategory("<category>", ...) block with a
specific reason.

Used during M1 → M2 triage to batch-update placeholder reasons after a
category has been root-caused. Idempotent: running twice with the same
arguments is a no-op.

Usage:
    relabel_skips.py --source resvg_test_suite.cc \
        --category "filters/filter-functions" \
        --reason "Bug: CSS filter function shorthand parses but is not applied"
"""

import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--category", required=True,
                        help="Category directory, e.g. 'filters/filter-functions'")
    parser.add_argument("--reason", required=True,
                        help="New reason string")
    args = parser.parse_args()

    src = args.source.read_text()

    needle = f'getTestsInCategory("{args.category}",'
    idx = src.find(needle)
    if idx < 0:
        print(f"error: category not found: {args.category}", file=sys.stderr)
        return 1

    # Find the matching getTestsInCategory(...) close paren so we don't bleed
    # into adjacent suites.
    open_paren = src.find("(", idx)
    depth = 0
    i = open_paren
    while i < len(src):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                close_paren = i
                break
        i += 1
    else:
        print(f"error: unbalanced parens in {args.category}", file=sys.stderr)
        return 1

    block = src[open_paren:close_paren + 1]
    # Escape any embedded double quotes in the reason for the C++ string.
    escaped = args.reason.replace("\\", "\\\\").replace('"', '\\"')
    new_block = block.replace(
        'Params::Skip("M1 upgrade: needs triage")',
        f'Params::Skip("{escaped}")',
    )
    n_replaced = (
        block.count('Params::Skip("M1 upgrade: needs triage")')
        - new_block.count('Params::Skip("M1 upgrade: needs triage")')
    )

    if n_replaced == 0:
        print(f"no placeholder skips found in {args.category}", file=sys.stderr)
        return 0

    src = src[:open_paren] + new_block + src[close_paren + 1:]
    args.source.write_text(src)
    print(f"relabeled {n_replaced} entries in {args.category}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
