#!/usr/bin/env python3
"""Decide whether an affected-target set contains instrumentable C/C++ code.

Reads the output of `bazel query --output label_kind` for the affected-target
set (as produced by the coverage lane's bazel-diff step, with aliases already
resolved to their `actual` targets) and reports whether any affected target is
an instrumentable C/C++ compilation unit.

The coverage lane uses this to skip PR coverage when a change's affected targets
are all non-instrumentable (docs, shell/python tooling, filegroups, build
flags), which would otherwise produce an empty LCOV report and trip the
"no executable line data" guard in check_lcov_report.py with a deterministic red
that no rerun can clear.

Fail-closed contract: the decision is `instrumentable_present=true` unless EVERY
classified target is a recognized, definitely-non-instrumentable kind. Any rule
kind outside the non-instrumentable allowlist (every cc_* kind, the donner C++
wrapper rules, and any kind this tool does not recognize) counts as
instrumentable, so an unknown or newly-added C++ rule keeps the coverage guard at
full strength rather than silently skipping it.

Host-instrumentability refinement: "instrumentable" means HOST-instrumentable.
Wasm-platform targets never produce host profile data: the emsdk wasm_cc_binary
wrapper is excluded by kind, and the underlying wasm cc_binary shim (whose kind
is indistinguishable from a host cc_binary) can be excluded by passing the
host-configuration cquery incompatibility result via --host-incompatible-file.
Labels absent from that file keep their kind-based classification, so a missing
or partial incompatibility list still fails closed toward running coverage.

Decide on the FINAL list, not on an intermediate one. The coverage lane narrows
its affected set (tests only, tag-filtered, variant-trimmed) before it runs, and
a decision taken on any wider set answers a question nobody asked: a non-test
helper or a tag-filtered sibling in the wider set can carry the whole subset
past the gate even though it never reaches the coverage command. Pass the final
list via --restrict-to-file and both criteria - rule kind AND host
compatibility - are applied to exactly the targets that will run. A final label
with no kind line is an incomplete answer, not a skippable one, so it fails
closed: the tool reports instrumentable_present=true and exits nonzero.
"""

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import sys

# Rule kinds that never contribute C/C++ line coverage. Skipping coverage is
# only allowed when the affected set consists ENTIRELY of these. Keep this list
# conservative: never add a kind that compiles or wraps C/C++, because a false
# entry here would silently disable the empty-report guard for real code.
NON_INSTRUMENTABLE_RULE_KINDS = frozenset(
    {
        # Python / shell / other-language rules.
        "py_test",
        "py_binary",
        "py_library",
        "sh_test",
        "sh_binary",
        "sh_library",
        "java_library",
        "java_binary",
        "java_test",
        "proto_library",
        # Grouping / query rules (no compilation of their own). NOTE: `alias`
        # is deliberately NOT listed. An alias can resolve to an instrumentable
        # C++ target (for example the default alias donner_variant_cc_test emits,
        # whose `actual` is a cc_test), so classifying `alias` as
        # non-instrumentable would wrongly skip coverage on a BUILD-only change
        # that only touches such an alias. The coverage lane resolves aliases to
        # their `actual` targets before calling this tool; any `alias` kind that
        # still reaches here could not be resolved and is treated as
        # instrumentable below (fail closed: uncertainty never skips the gate).
        "filegroup",
        "genrule",
        "genquery",
        "test_suite",
        "serve_http",
        "web_package",
        # Configuration, flags, and platform/toolchain declarations.
        "config_setting",
        "bool_flag",
        "int_flag",
        "string_flag",
        "label_flag",
        "label_setting",
        "platform",
        "constraint_setting",
        "constraint_value",
        "toolchain",
        "toolchain_type",
        # Internal helper rules that emit files but no instrumented objects.
        "_expand_template",
        "_check_python_version",
        # Wasm-platform packaging rules. The emsdk wasm_cc_binary wrapper (kind
        # `_wasm_cc_binary`; both spellings listed defensively) transitions its
        # cc_target to the wasm platform and only ever emits .js/.wasm
        # artifacts. It can never contribute HOST profile data, so lcov can
        # never see it, and running coverage over a wasm-only affected set
        # yields an empty report that trips the "no executable line data"
        # guard: a deterministic false RUN. The underlying `cc_binary` shim
        # (target_compatible_with @platforms//:incompatible on the host) is NOT
        # listed here: its kind is indistinguishable from a host cc_binary, so
        # the coverage lane resolves it with a host-compatibility cquery and
        # feeds the result in via --host-incompatible-file instead.
        "_wasm_cc_binary",
        "wasm_cc_binary",
    }
)

# `bazel query --output label_kind` phrases for non-rule targets. These never
# carry coverage either.
NON_RULE_PHRASES = frozenset(
    {
        "source file",
        "generated file",
        "package group",
        "environment group",
    }
)


@dataclass
class Classification:
    """Bucketed classification of an affected-target set."""

    instrumentable: list[str] = field(default_factory=list)
    non_instrumentable: list[str] = field(default_factory=list)

    @property
    def total(self) -> int:
        return len(self.instrumentable) + len(self.non_instrumentable)

    @property
    def instrumentable_present(self) -> bool:
        # Fail closed: an empty classification (e.g. every target dropped by a
        # keep-going query) is treated as instrumentable so coverage still runs.
        if self.total == 0:
            return True
        return len(self.instrumentable) > 0


def _split_kind_and_label(line: str) -> tuple[str, str]:
    """Split a `--output label_kind` line into (kind phrase, label).

    Args:
        line: A single non-empty output line.

    Returns:
        The kind phrase (e.g. "cc_library rule", "source file") and the label.
    """
    tokens = line.split()
    label = tokens[-1]
    kind_phrase = " ".join(tokens[:-1])
    return kind_phrase, label


def normalize_label(label: str) -> str:
    """Normalize a bazel label for comparison across query/cquery output.

    `bazel cquery` prints canonical labels (`@@//pkg:name`) while `bazel query`
    prints apparent ones (`//pkg:name`). Both refer to the main repository.

    Args:
        label: A bazel target label.

    Returns:
        The label with a leading `@@` or `@` main-repo prefix stripped.
    """
    if label.startswith("@@//"):
        return label[2:]
    if label.startswith("@//"):
        return label[1:]
    return label


def restrict_lines(
    lines: list[str], labels: list[str]
) -> tuple[list[str], list[str]]:
    """Select the label_kind lines for exactly `labels`, in `labels` order.

    Args:
        lines: Raw lines from `bazel query --output label_kind` for a set that
            is a superset of `labels` (the coverage lane's alias-resolved
            affected set).
        labels: The final coverage target list, one label per entry.

    Returns:
        (selected lines, labels that had no kind line). A non-empty second
        element means the caller cannot decide on this set: `tests()` expansion
        can name a test_suite member that was never in the queried set, and an
        unclassified target must keep the coverage run rather than skip it.
    """
    by_label: dict[str, str] = {}
    for raw_line in lines:
        line = raw_line.strip()
        if not line:
            continue
        _, label = _split_kind_and_label(line)
        by_label.setdefault(normalize_label(label), line)

    selected: list[str] = []
    missing: list[str] = []
    for raw_label in labels:
        label = normalize_label(raw_label.strip())
        if not label:
            continue
        line = by_label.get(label)
        if line is None:
            missing.append(label)
        else:
            selected.append(line)
    return selected, missing


def classify(
    lines: list[str], host_incompatible: frozenset[str] = frozenset()
) -> Classification:
    """Classify label_kind output lines into instrumentable vs. not.

    Args:
        lines: Raw lines from `bazel query --output label_kind`.
        host_incompatible: Normalized labels that a host-configuration cquery
            reported as IncompatiblePlatformProvider (e.g. wasm-only cc_binary
            shims). These can never contribute host coverage, so they classify
            as non-instrumentable regardless of kind. Labels NOT in this set
            keep their kind-based classification (fail closed).

    Returns:
        The bucketed classification.
    """
    result = Classification()
    for raw_line in lines:
        line = raw_line.strip()
        if not line:
            continue
        kind_phrase, label = _split_kind_and_label(line)
        if not kind_phrase:
            # No kind phrase (malformed): fail closed and treat as instrumentable.
            result.instrumentable.append(label)
            continue
        if normalize_label(label) in host_incompatible:
            # Host-incompatible targets (e.g. a wasm bridge cc_binary marked
            # target_compatible_with @platforms//:incompatible on the host)
            # never produce host profile data; bazel's
            # --skip_incompatible_explicit_targets silently drops them from the
            # coverage build, so counting them as instrumentable would force a
            # run whose report is guaranteed empty.
            result.non_instrumentable.append(label)
            continue
        if kind_phrase in NON_RULE_PHRASES:
            result.non_instrumentable.append(label)
            continue
        if kind_phrase.endswith(" rule"):
            rule_kind = kind_phrase[: -len(" rule")]
            if rule_kind in NON_INSTRUMENTABLE_RULE_KINDS:
                result.non_instrumentable.append(label)
            else:
                # Every cc_* kind, the donner C++ wrapper rules, and any
                # unrecognized rule kind land here: run coverage (fail closed).
                result.instrumentable.append(label)
            continue
        # Unrecognized phrase shape: fail closed.
        result.instrumentable.append(label)
    return result


def _print_summary(classification: Classification, preview: int = 25) -> None:
    print(
        "Coverage instrumentability classification: "
        f"total={classification.total} "
        f"instrumentable={len(classification.instrumentable)} "
        f"non_instrumentable={len(classification.non_instrumentable)}",
        file=sys.stderr,
    )
    if classification.instrumentable:
        print("Instrumentable (forces coverage run):", file=sys.stderr)
        for label in classification.instrumentable[:preview]:
            print(f"  {label}", file=sys.stderr)
        if len(classification.instrumentable) > preview:
            remaining = len(classification.instrumentable) - preview
            print(f"  ... and {remaining} more", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--label-kind-file",
        type=Path,
        required=True,
        help="File containing `bazel query --output label_kind` output.",
    )
    parser.add_argument(
        "--restrict-to-file",
        type=Path,
        default=None,
        help=(
            "Optional file of labels (one per line) naming the FINAL coverage "
            "target list. Classification is restricted to exactly those "
            "targets; a label with no kind line fails closed (reports "
            "instrumentable_present=true and exits nonzero)."
        ),
    )
    parser.add_argument(
        "--host-incompatible-file",
        type=Path,
        default=None,
        help=(
            "Optional file of labels (one per line) that a host-configuration "
            "cquery reported as incompatible (IncompatiblePlatformProvider). "
            "These classify as non-instrumentable regardless of kind."
        ),
    )
    args = parser.parse_args()

    try:
        text = args.label_kind_file.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        # Fail closed: emit the run decision and a nonzero status so the caller
        # runs coverage with the guard intact.
        print("instrumentable_present=true")
        print(f"ERROR: could not read {args.label_kind_file}: {error}", file=sys.stderr)
        return 1

    host_incompatible: frozenset[str] = frozenset()
    if args.host_incompatible_file is not None:
        try:
            host_incompatible = frozenset(
                normalize_label(line.strip())
                for line in args.host_incompatible_file.read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()
                if line.strip()
            )
        except OSError as error:
            # Fail closed: an unreadable incompatibility list must never skip
            # the gate, so classify without it (targets stay instrumentable).
            print(
                f"WARNING: could not read {args.host_incompatible_file}: {error}; "
                "classifying without host-incompatibility data.",
                file=sys.stderr,
            )

    lines = text.splitlines()
    if args.restrict_to_file is not None:
        try:
            final_labels = args.restrict_to_file.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
        except OSError as error:
            print("instrumentable_present=true")
            print(
                f"ERROR: could not read {args.restrict_to_file}: {error}",
                file=sys.stderr,
            )
            return 1
        lines, missing = restrict_lines(lines, final_labels)
        if missing:
            # Fail closed: an unclassified member of the final list means the
            # answer is unknown, and unknown must never skip the guard.
            print("instrumentable_present=true")
            print(
                "ERROR: no kind line for %d of the final target(s): %s"
                % (len(missing), ", ".join(missing[:5])),
                file=sys.stderr,
            )
            return 1

    classification = classify(lines, host_incompatible)
    _print_summary(classification)

    value = "true" if classification.instrumentable_present else "false"
    print(f"instrumentable_present={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
