#!/usr/bin/env python3
"""Validate that an LCOV report contains executable line coverage data."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys


@dataclass(frozen=True)
class LcovStats:
    """Summary counters extracted from an LCOV report."""

    records: int
    source_files: int
    line_entries: int
    found_lines: int
    hit_lines: int


def collect_lcov_stats(path: Path) -> LcovStats:
    """Collect high-level counters from an LCOV report.

    Args:
        path: LCOV report path.

    Returns:
        Parsed LCOV counters.
    """
    records = 0
    source_files = 0
    line_entries = 0
    found_lines = 0
    hit_lines = 0

    with path.open(encoding="utf-8", errors="replace") as lcov_file:
        for raw_line in lcov_file:
            line = raw_line.rstrip("\n")
            if line.startswith("SF:"):
                source_files += 1
            elif line.startswith("DA:"):
                line_entries += 1
            elif line.startswith("LF:"):
                found_lines += _parse_counter(line, "LF:")
            elif line.startswith("LH:"):
                hit_lines += _parse_counter(line, "LH:")
            elif line == "end_of_record":
                records += 1

    return LcovStats(
        records=records,
        source_files=source_files,
        line_entries=line_entries,
        found_lines=found_lines,
        hit_lines=hit_lines,
    )


def validate_lcov_report(path: Path) -> LcovStats:
    """Validate that an LCOV report is usable by Codecov.

    Args:
        path: LCOV report path.

    Returns:
        Parsed LCOV counters when the report is usable.

    Raises:
        ValueError: If the report is missing executable line data.
    """
    stats = collect_lcov_stats(path)
    if stats.source_files == 0 or stats.line_entries == 0 or stats.found_lines == 0:
        raise ValueError(
            "Coverage report has no executable line data "
            f"(records={stats.records}, source_files={stats.source_files}, "
            f"DA={stats.line_entries}, LF={stats.found_lines}, LH={stats.hit_lines})."
        )

    return stats


def _parse_counter(line: str, prefix: str) -> int:
    value = line[len(prefix) :].strip()
    try:
        return int(value)
    except ValueError:
        return 0


def _print_report_preview(path: Path, max_lines: int = 60) -> None:
    print("First LCOV lines:", file=sys.stderr)
    with path.open(encoding="utf-8", errors="replace") as lcov_file:
        for index, line in enumerate(lcov_file):
            if index >= max_lines:
                break
            print(line.rstrip("\n"), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="LCOV report to validate")
    args = parser.parse_args()

    try:
        stats = validate_lcov_report(args.report)
    except FileNotFoundError:
        print(f"ERROR: Coverage report not found: {args.report}", file=sys.stderr)
        return 1
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        _print_report_preview(args.report)
        return 1

    print(
        "Coverage report contains executable line data: "
        f"records={stats.records} source_files={stats.source_files} "
        f"DA={stats.line_entries} LF={stats.found_lines} LH={stats.hit_lines}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
