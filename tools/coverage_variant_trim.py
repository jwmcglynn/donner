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

Fail-safe contract: a target is dropped ONLY when ALL of the following hold:
(1) its label carries a variant suffix, (2) its base sibling (the label with
the suffix stripped) is itself present in the affected set, and (3) its rule
kind, per the caller-provided `bazel query --output label_kind` result, is the
generated wrapper kind (`donner_multi_transitioned_test`). The kind gate keeps
handwritten targets that merely share the naming convention (e.g. a
`cc_library` named `renderer_geode` beside `renderer`) out of the trim. A
label missing from the kind file, an unreadable kind file, or any error in
this tool must be treated as "keep the target" / "keep the full set" (fail
closed toward more coverage).

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

# The rule kind donner_cc_test's `variants` attr generates for each
# `{name}_{variant}` wrapper (build_defs/rules.bzl). Only this kind is ever
# trimmed.
WRAPPER_KIND = "donner_multi_transitioned_test"


def parse_label_kinds(label_kind_text):
    """Parse `bazel query --output label_kind` text into {label: kind}.

    Lines look like `donner_multi_transitioned_test rule //pkg:name`. Lines
    that do not match are skipped (fail-safe: unknown labels are not trimmed).
    """
    kinds = {}
    for line in label_kind_text.splitlines():
        parts = line.strip().split(" ")
        if len(parts) >= 3 and parts[-2] == "rule":
            kinds[parts[-1]] = " ".join(parts[:-2])
    return kinds


def trim_variants(labels, label_kinds=None):
    """Return (kept, dropped) lists. See module docstring for the policy.

    label_kinds: {label: kind} from parse_label_kinds, or None. When None or
    when a label is missing from it, that label is NEVER trimmed (fail-safe).
    """
    label_set = set(labels)
    kinds = label_kinds or {}
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
                if (
                    base_name
                    and base in label_set
                    and kinds.get(label) == WRAPPER_KIND
                ):
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
    parser.add_argument(
        "--label-kind-file",
        help="`bazel query --output label_kind` result for the affected set. "
        "REQUIRED for any trimming to happen: labels without a known "
        "wrapper kind are never dropped (fail-safe).",
    )
    args = parser.parse_args(argv)

    labels = [
        line.strip()
        for line in Path(args.affected_file).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]

    label_kinds = None
    if args.label_kind_file:
        try:
            label_kinds = parse_label_kinds(
                Path(args.label_kind_file).read_text(encoding="utf-8")
            )
        except OSError as error:
            sys.stderr.write(
                f"variant trim: kind file unreadable ({error}); trimming nothing\n"
            )
            label_kinds = None

    kept, dropped = trim_variants(labels, label_kinds)

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
