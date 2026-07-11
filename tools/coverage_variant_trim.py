#!/usr/bin/env python3
"""Trim redundant variant-wrapper targets from the PR coverage target set.

Policy (operator-approved, 2026-07-11): PR PATCH coverage instruments and runs
ONE representative variant per test - the base/default target - and the full
multi-variant matrix stays on the main-push coverage baseline. Rationale: the
`donner_variant_cc_test` wrappers (`{name}_tiny`, `{name}_text_full`,
`{name}_geode`; see build_defs/rules.bzl `_VARIANT_SPECS`) compile the SAME
source files under different backend/feature flags, so for Codecov patch
coverage of a PR's changed lines they are redundant parameterizations - their
unique coverage is limited to backend/feature-gated lines, which the main-push
full run keeps covering. The measured cost of the redundancy on the coverage
lane is a ~7x instrumented compile multiplier on SVG element translation units
and 90 of 296 test executions (docs/design_docs/0029-2, PR #829 exemplar).

The base variant is the representative because it is the default build graph
(what `bazel test` and the CI lanes exercise by default), it already accounts
for the majority of coverage test executions (206 of 296 on the exemplar), and
its instrumented surface is the broadest default-feature surface.

Fail-safe contract: a variant-suffixed target is dropped ONLY when its base
sibling (the label with the suffix stripped) is itself present in the affected
set, so every test's coverage keeps at least one representative in the PR run.
Variant-only entries (no base sibling in the set) are kept. Non-variant labels
are never touched. Any error in this tool must be treated by the caller as
"keep the full set" (fail closed).

Reads one label per line, writes the trimmed set (one per line) to stdout and
a summary line to stderr.
"""

import argparse
from pathlib import Path
import sys

# Keep in sync with build_defs/rules.bzl _VARIANT_SPECS. Order matters only
# for readability; suffixes are tested longest-first to keep `_text_full`
# from being misread as a `_full` variant of `..._text`.
VARIANT_SUFFIXES = ("_text_full", "_tiny", "_geode")


def trim_variants(labels):
    """Return (kept, dropped) lists. See module docstring for the policy."""
    label_set = set(labels)
    kept = []
    dropped = []
    for label in labels:
        drop = False
        for suffix in VARIANT_SUFFIXES:
            if label.endswith(suffix):
                base = label[: -len(suffix)]
                # Only a real target name qualifies (guard against a label
                # that IS the suffix, e.g. "//pkg:_geode").
                base_name = base.rsplit(":", 1)[-1]
                if base_name and base in label_set:
                    drop = True
                break
        (dropped if drop else kept).append(label)
    return kept, dropped


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--affected-file",
        required=True,
        help="File with one affected target label per line.",
    )
    parser.add_argument(
        "--dropped-out",
        help="Optional file to record the dropped variant labels.",
    )
    args = parser.parse_args(argv)

    labels = [
        line.strip()
        for line in Path(args.affected_file).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    kept, dropped = trim_variants(labels)

    if args.dropped_out:
        Path(args.dropped_out).write_text(
            "".join(f"{label}\n" for label in dropped), encoding="utf-8"
        )

    sys.stdout.write("".join(f"{label}\n" for label in kept))
    sys.stderr.write(
        f"variant trim: kept={len(kept)} dropped={len(dropped)} of {len(labels)}\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
