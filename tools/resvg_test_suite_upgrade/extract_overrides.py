#!/usr/bin/env python3
"""Extract override entries from resvg_test_suite.cc into a structured JSON.

Each override is `{"filename.svg", Params::Something(args)}, // comment`,
possibly spanning multiple lines. The output JSON is consumed by the codemod
that rewrites the test file for the new resvg-test-suite layout.

Output schema:
    {
      "overrides": [
        {
          "filename": "a-fill-010.svg",
          "params": "Params::Skip()",
          "comment": "UB: rgb(int int int)",
          "suite_name": "Fill",
          "default_params": null,   # or "kFilterDefaultParams" etc.
          "kwargs": []              # any trailing modifiers like .onlyTextFull()
        },
        ...
      ],
      "default_suites": [           # suites with no overrides at all
        {"suite_name": "AlignmentBaseline", "prefix": "a-alignment-baseline"},
        ...
      ],
      "preamble": "...",            # the leading file content up to first suite
      "inter_suite_comments": [     # comments between suites, for reference
        {"after_suite": "DominantBaseline", "text": "..."},
        ...
      ]
    }

Intentionally uses regex + line walking rather than a C++ parser — the file is
mechanically structured enough that regex works, and we want to preserve the
exact spelling of Params expressions and comments.
"""

import argparse
import json
import re
import sys
from pathlib import Path
from dataclasses import dataclass, field, asdict


SUITE_START_RE = re.compile(
    r"INSTANTIATE_TEST_SUITE_P\(\s*(?P<name>\w+)\s*,\s*ImageComparisonTestFixture,"
)
PREFIX_RE = re.compile(r'getTestsWithPrefix\(\s*"(?P<prefix>[^"]+)"')
# Match {"filename.svg", Params::Something(...)} — handle balanced parens in params.
# Comment (trailing // text) is optional and captured separately.
ENTRY_START_RE = re.compile(r'\{\s*"(?P<filename>[^"]+\.svg)"\s*,')
COMMENT_RE = re.compile(r"//\s*(?P<text>.*)$")


@dataclass
class OverrideEntry:
    filename: str
    params: str
    comment: str
    suite_name: str
    suite_prefix: str


def extract_balanced(text: str, start: int, open_ch: str, close_ch: str) -> int:
    """Given text[start] == open_ch, return the index AFTER the matching close."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError(f"unbalanced {open_ch}{close_ch} from index {start}")


def parse(source: str) -> dict:
    """Walk the source and extract entries.

    Approach: find each INSTANTIATE_TEST_SUITE_P(...) call by scanning for
    the macro name and expanding to the matching ')'. Within that chunk,
    extract the suite name, prefix, and any override entries.
    """
    overrides: list[OverrideEntry] = []
    default_suites: list[dict] = []

    # Find every INSTANTIATE_TEST_SUITE_P block
    for m in SUITE_START_RE.finditer(source):
        suite_name = m.group("name")
        # Find the opening '(' right after INSTANTIATE_TEST_SUITE_P
        # Actually, m.start() is at 'I'. Find the '(' we need to balance from
        # the first '(' in the match.
        paren_start = source.index("(", m.start())
        block_end = extract_balanced(source, paren_start, "(", ")")
        block = source[paren_start:block_end]

        prefix_m = PREFIX_RE.search(block)
        if not prefix_m:
            continue
        prefix = prefix_m.group("prefix")

        # Look for override entries inside the getTestsWithPrefix(...) call.
        # Find the '{' that opens the overrides map. It's the one after
        # `"prefix"` followed by a ',' and optional whitespace.
        after_prefix = block.find('"', prefix_m.start()) + len(prefix) + 2
        # Simpler: find every ENTRY_START_RE match within the block.
        for em in ENTRY_START_RE.finditer(block):
            filename = em.group("filename")
            # Skip the prefix string itself if it happens to match (it won't
            # because we require .svg suffix, but be safe).
            # Extract the params expression: everything from the ',' after
            # filename to the matching '}'.
            comma_idx = em.end()
            # Skip whitespace to find the start of the params expression.
            i = comma_idx
            while i < len(block) and block[i] in " \t\n":
                i += 1
            params_start = i
            # Find the closing '}' of this entry. Balance '{' '}' from the
            # entry's opening '{' at em.start().
            entry_end = extract_balanced(block, em.start(), "{", "}")
            # The params span is [params_start : entry_end - 1] (before '}').
            params_expr = block[params_start:entry_end - 1].rstrip()
            # Collapse multi-line params into a single line for cleanliness.
            params_expr = re.sub(r"\s+", " ", params_expr).strip()

            # Find any trailing // comment after entry_end on the same or
            # next immediate line.
            rest = block[entry_end:]
            comment = ""
            # Look for ',' then optional whitespace then '//'
            tail_m = re.match(r"\s*,?\s*(?://\s*(.+?))?(?:\n|$)", rest)
            if tail_m and tail_m.group(1):
                comment = tail_m.group(1).strip()

            overrides.append(OverrideEntry(
                filename=filename,
                params=params_expr,
                comment=comment,
                suite_name=suite_name,
                suite_prefix=prefix,
            ))

        # Track suites with no overrides for the "default_suites" list
        if not any(o.suite_name == suite_name for o in overrides):
            default_suites.append({"suite_name": suite_name, "prefix": prefix})

    return {
        "overrides": [asdict(o) for o in overrides],
        "suite_list": [
            {"suite_name": sn, "prefix": pf}
            for sn, pf in dict.fromkeys(
                (o.suite_name, o.suite_prefix) for o in overrides
            )
        ],
        "default_suites": default_suites,
        "total_overrides": len(overrides),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    source = args.source.read_text()
    result = parse(source)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")

    print(f"Extracted {result['total_overrides']} overrides", file=sys.stderr)
    # Stats on Params types
    from collections import Counter
    param_types = Counter()
    for o in result["overrides"]:
        # Get the first "Params::Something" token
        m = re.match(r"(Params::\w+|Params\(\))", o["params"])
        if m:
            param_types[m.group(1)] += 1
        else:
            param_types["<other>"] += 1
    for t, c in param_types.most_common():
        print(f"  {t}: {c}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
