#!/usr/bin/env python3
"""Report Codecov-relevant LCOV line and branch coverage metrics."""

import argparse
from dataclasses import dataclass, field
import json
from math import ceil
from pathlib import Path
import sys
from typing import Any


@dataclass(frozen=True)
class CoverageCounts:
    """Hit/found coverage counts."""

    hit: int = 0
    found: int = 0

    @property
    def percent(self) -> float:
        """Return hit/found as a percentage, or 100 for an empty denominator."""
        if self.found == 0:
            return 100.0
        return self.hit / self.found * 100.0

    def hits_needed_for(self, target_percent: float) -> int:
        """Return additional hits needed to reach target_percent for this denominator."""
        if self.found == 0:
            return 0
        target_hits = ceil(target_percent / 100.0 * self.found)
        return max(0, target_hits - self.hit)


@dataclass(frozen=True)
class FileCoverage:
    """Coverage counters for one LCOV source file record."""

    source_file: str
    lines: CoverageCounts = field(default_factory=CoverageCounts)
    branches: CoverageCounts = field(default_factory=CoverageCounts)

    @property
    def missed_branches(self) -> int:
        """Return uncovered branch counters for this file."""
        return self.branches.found - self.branches.hit


@dataclass(frozen=True)
class LcovMetrics:
    """Coverage counters for an LCOV report."""

    files: list[FileCoverage]
    lines: CoverageCounts
    branches: CoverageCounts


def collect_lcov_metrics(path: Path) -> LcovMetrics:
    """Collect line and branch coverage counters from an LCOV report."""
    files: list[FileCoverage] = []
    source_file: str | None = None
    line_found = 0
    line_hit = 0
    branch_found = 0
    branch_hit = 0

    def flush_record() -> None:
        nonlocal source_file, line_found, line_hit, branch_found, branch_hit
        if source_file is not None:
            files.append(
                FileCoverage(
                    source_file=source_file,
                    lines=CoverageCounts(hit=line_hit, found=line_found),
                    branches=CoverageCounts(hit=branch_hit, found=branch_found),
                )
            )
        source_file = None
        line_found = 0
        line_hit = 0
        branch_found = 0
        branch_hit = 0

    with path.open(encoding="utf-8", errors="replace") as lcov_file:
        for raw_line in lcov_file:
            line = raw_line.rstrip("\n")
            if line.startswith("SF:"):
                flush_record()
                source_file = line[3:]
            elif line.startswith("DA:"):
                line_found += 1
                if _parse_da_hit(line) > 0:
                    line_hit += 1
            elif line.startswith("BRDA:"):
                branch_found += 1
                if _parse_brda_hit(line) > 0:
                    branch_hit += 1
            elif line == "end_of_record":
                flush_record()

    flush_record()
    return LcovMetrics(
        files=files,
        lines=CoverageCounts(
            hit=sum(file.lines.hit for file in files),
            found=sum(file.lines.found for file in files),
        ),
        branches=CoverageCounts(
            hit=sum(file.branches.hit for file in files),
            found=sum(file.branches.found for file in files),
        ),
    )


def _parse_da_hit(line: str) -> int:
    try:
        return int(line.split(",", 1)[1])
    except (IndexError, ValueError):
        return 0


def _parse_brda_hit(line: str) -> int:
    try:
        value = line.rsplit(",", 1)[1]
    except IndexError:
        return 0
    if value == "-":
        return 0
    try:
        return int(value)
    except ValueError:
        return 0


def metrics_to_json(metrics: LcovMetrics, branch_target: float, top_misses: int) -> dict[str, Any]:
    """Return a JSON-serializable metrics summary."""
    top_files = sorted(metrics.files, key=lambda file: file.missed_branches, reverse=True)
    return {
        "source_files": len(metrics.files),
        "lines": {
            "hit": metrics.lines.hit,
            "found": metrics.lines.found,
            "percent": metrics.lines.percent,
        },
        "branches": {
            "hit": metrics.branches.hit,
            "found": metrics.branches.found,
            "percent": metrics.branches.percent,
            "target_percent": branch_target,
            "hits_needed_for_target": metrics.branches.hits_needed_for(branch_target),
        },
        "top_branch_misses": [
            {
                "source_file": file.source_file,
                "missed": file.missed_branches,
                "hit": file.branches.hit,
                "found": file.branches.found,
                "percent": file.branches.percent,
            }
            for file in top_files
            if file.missed_branches > 0
        ][:top_misses],
    }


def print_text_summary(metrics: LcovMetrics, branch_target: float, top_misses: int) -> None:
    """Print a compact human-readable coverage summary."""
    print("Filtered LCOV coverage metrics:")
    print(f"  source files: {len(metrics.files)}")
    print(
        f"  lines: {metrics.lines.hit}/{metrics.lines.found} "
        f"({metrics.lines.percent:.2f}%)"
    )
    if metrics.branches.found == 0:
        print("  branches: no branch counters in this report")
    else:
        print(
            f"  branches: {metrics.branches.hit}/{metrics.branches.found} "
            f"({metrics.branches.percent:.2f}%)"
        )
        print(
            f"  branches to {branch_target:g}%: "
            f"{metrics.branches.hits_needed_for(branch_target)}"
        )

    top_files = sorted(metrics.files, key=lambda file: file.missed_branches, reverse=True)
    printed = 0
    for file in top_files:
        if file.missed_branches <= 0:
            continue
        if printed == 0:
            print("  top branch misses:")
        print(
            f"    {file.missed_branches:4d} "
            f"{file.branches.hit:4d}/{file.branches.found:<4d} {file.source_file}"
        )
        printed += 1
        if printed >= top_misses:
            break


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="Filtered LCOV report to summarize")
    parser.add_argument(
        "--branch-target",
        type=float,
        default=85.0,
        help="Branch coverage target percentage to compute the hit shortfall for",
    )
    parser.add_argument(
        "--top-misses",
        type=int,
        default=20,
        help="Number of source files with the most missed branch counters to print",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    args = parser.parse_args()

    try:
        metrics = collect_lcov_metrics(args.report)
    except FileNotFoundError:
        print(f"ERROR: Coverage report not found: {args.report}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(metrics_to_json(metrics, args.branch_target, args.top_misses), indent=2))
    else:
        print_text_summary(metrics, args.branch_target, args.top_misses)
    return 0


if __name__ == "__main__":
    sys.exit(main())
