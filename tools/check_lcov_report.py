#!/usr/bin/env python3
"""Validate that an LCOV report contains executable line coverage data."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


@dataclass(frozen=True)
class LcovStats:
    """Summary counters extracted from an LCOV report."""

    records: int
    source_files: int
    line_entries: int
    found_lines: int
    hit_lines: int


def _validate_source_path(source: str) -> None:
    source_path = Path(source)
    if (
        not re.fullmatch(r"donner/[A-Za-z0-9_./+\-]+", source)
        or source_path.is_absolute()
        or ".." in source_path.parts
    ):
        raise ValueError("Coverage report contains a non-public source path")


def _line_counters(line: str) -> tuple[int, int, int, int, int]:
    if line.startswith("SF:"):
        _validate_source_path(line[3:].strip())
        return 0, 1, 0, 0, 0
    if line.startswith("DA:"):
        return 0, 0, 1, 0, 0
    if line.startswith("LF:"):
        return 0, 0, 0, _parse_counter(line, "LF:"), 0
    if line.startswith("LH:"):
        return 0, 0, 0, 0, _parse_counter(line, "LH:")
    if line == "end_of_record":
        return 1, 0, 0, 0, 0
    return 0, 0, 0, 0, 0


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
            record, source, line_entry, found, hit = _line_counters(raw_line.rstrip("\n"))
            records += record
            source_files += source
            line_entries += line_entry
            found_lines += found
            hit_lines += hit

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
        return 1

    print(
        "Coverage report contains executable line data: "
        f"records={stats.records} source_files={stats.source_files} "
        f"DA={stats.line_entries} LF={stats.found_lines} LH={stats.hit_lines}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
