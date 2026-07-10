#!/usr/bin/env python3
"""Decide whether an affected-target set contains instrumentable C/C++ code.

Reads the output of `bazel query --output label_kind` for the affected-target
set (as produced by the coverage lane's bazel-diff step) and reports whether any
affected target is an instrumentable C/C++ compilation unit.

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
        # Grouping / aliasing / query rules (no compilation of their own).
        "filegroup",
        "genrule",
        "genquery",
        "alias",
        "test_suite",
        "serve_http",
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


def classify(lines: list[str]) -> Classification:
    """Classify label_kind output lines into instrumentable vs. not.

    Args:
        lines: Raw lines from `bazel query --output label_kind`.

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
    args = parser.parse_args()

    try:
        text = args.label_kind_file.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        # Fail closed: emit the run decision and a nonzero status so the caller
        # runs coverage with the guard intact.
        print("instrumentable_present=true")
        print(f"ERROR: could not read {args.label_kind_file}: {error}", file=sys.stderr)
        return 1

    classification = classify(text.splitlines())
    _print_summary(classification)
    value = "true" if classification.instrumentable_present else "false"
    print(f"instrumentable_present={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
