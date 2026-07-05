#!/usr/bin/env python3
"""Report Codecov-style LCOV line and branch coverage metrics.

Codecov's LCOV summary reports line buckets as hits, misses, and partials. A
line with a positive DA counter is still partial, not a hit, when any BRDA
counter on that source line is uncovered. The Codecov percentage is therefore:

    hits / (hits + misses + partials)

This differs from raw LCOV line coverage, which counts every positive DA line
as covered.
"""

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
class CodecovLineCounts:
    """Codecov-style line buckets: fully covered, missed, and partial lines."""

    hits: int = 0
    misses: int = 0
    partials: int = 0

    @property
    def total(self) -> int:
        """Return total coverable lines."""
        return self.hits + self.misses + self.partials

    @property
    def percent(self) -> float:
        """Return Codecov-style line coverage percentage."""
        if self.total == 0:
            return 100.0
        return self.hits / self.total * 100.0

    @property
    def display_percent(self) -> int:
        """Return the whole-number percentage shown by Codecov's project UI."""
        return int(self.percent + 0.5)

    def hits_needed_for(self, target_percent: float) -> int:
        """Return additional fully covered lines needed to reach target_percent."""
        if self.total == 0:
            return 0
        target_hits = ceil(target_percent / 100.0 * self.total)
        return max(0, target_hits - self.hits)


@dataclass(frozen=True)
class FileCoverage:
    """Coverage counters for one LCOV source file record."""

    source_file: str
    lines: CoverageCounts = field(default_factory=CoverageCounts)
    codecov_lines: CodecovLineCounts = field(default_factory=CodecovLineCounts)
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
    codecov_lines: CodecovLineCounts
    branches: CoverageCounts


def collect_lcov_metrics(
    path: Path, reference_lines: dict[str, set[int]] | None = None
) -> LcovMetrics:
    """Collect line and branch coverage counters from an LCOV report."""
    files: list[FileCoverage] = []
    source_file: str | None = None
    line_hits: dict[int, int] = {}
    branch_hits_by_line: dict[int, list[int]] = {}
    seen_reference_files: set[str] = set()

    def flush_record() -> None:
        nonlocal source_file, line_hits, branch_hits_by_line, seen_reference_files
        if source_file is not None:
            if reference_lines is not None:
                expected_lines = reference_lines.get(source_file)
                if expected_lines is None:
                    source_file = None
                    line_hits = {}
                    branch_hits_by_line = {}
                    return

                seen_reference_files.add(source_file)
                line_hits = {
                    line_no: line_hits.get(line_no, 0) for line_no in sorted(expected_lines)
                }
                branch_hits_by_line = {
                    line_no: hits
                    for line_no, hits in branch_hits_by_line.items()
                    if line_no in expected_lines
                }

            line_counts = _collect_line_counts(line_hits)
            codecov_line_counts = _collect_codecov_line_counts(line_hits, branch_hits_by_line)
            branch_counts = _collect_branch_counts(branch_hits_by_line)
            files.append(
                FileCoverage(
                    source_file=source_file,
                    lines=line_counts,
                    codecov_lines=codecov_line_counts,
                    branches=branch_counts,
                )
            )
        source_file = None
        line_hits = {}
        branch_hits_by_line = {}

    with path.open(encoding="utf-8", errors="replace") as lcov_file:
        for raw_line in lcov_file:
            line = raw_line.rstrip("\n")
            if line.startswith("SF:"):
                flush_record()
                source_file = line[3:]
            elif line.startswith("DA:"):
                line_no, hit_count = _parse_da(line)
                if line_no is not None:
                    line_hits[line_no] = hit_count
            elif line.startswith("BRDA:"):
                line_no, hit_count = _parse_brda(line)
                if line_no is not None:
                    branch_hits_by_line.setdefault(line_no, []).append(hit_count)
            elif line == "end_of_record":
                flush_record()

    flush_record()
    if reference_lines is not None:
        for reference_file in sorted(set(reference_lines) - seen_reference_files):
            line_hits = {line_no: 0 for line_no in sorted(reference_lines[reference_file])}
            files.append(
                FileCoverage(
                    source_file=reference_file,
                    lines=_collect_line_counts(line_hits),
                    codecov_lines=_collect_codecov_line_counts(line_hits, {}),
                    branches=CoverageCounts(),
                )
            )

    return LcovMetrics(
        files=files,
        lines=CoverageCounts(
            hit=sum(file.lines.hit for file in files),
            found=sum(file.lines.found for file in files),
        ),
        codecov_lines=CodecovLineCounts(
            hits=sum(file.codecov_lines.hits for file in files),
            misses=sum(file.codecov_lines.misses for file in files),
            partials=sum(file.codecov_lines.partials for file in files),
        ),
        branches=CoverageCounts(
            hit=sum(file.branches.hit for file in files),
            found=sum(file.branches.found for file in files),
        ),
    )


def load_codecov_reference_lines(path: Path) -> dict[str, set[int]]:
    """Load the file/line universe from a Codecov commit API JSON response."""
    data = json.loads(path.read_text(encoding="utf-8"))
    report = data.get("report", data)
    files = report.get("files", [])
    reference_lines: dict[str, set[int]] = {}
    for file_entry in files:
        source_file = file_entry.get("name")
        line_coverage = file_entry.get("line_coverage", [])
        if not isinstance(source_file, str):
            continue

        lines: set[int] = set()
        for line_entry in line_coverage:
            if not isinstance(line_entry, list) or not line_entry:
                continue
            try:
                lines.add(int(line_entry[0]))
            except (TypeError, ValueError):
                continue

        reference_lines[source_file] = lines

    return reference_lines


def _collect_line_counts(line_hits: dict[int, int]) -> CoverageCounts:
    return CoverageCounts(
        hit=sum(1 for hit_count in line_hits.values() if hit_count > 0),
        found=len(line_hits),
    )


def _collect_codecov_line_counts(
    line_hits: dict[int, int], branch_hits_by_line: dict[int, list[int]]
) -> CodecovLineCounts:
    """Return Codecov-style line buckets from DA and BRDA counters.

    Codecov's LCOV UI keeps branch-partial lines in the line denominator but
    does not include them in the hit numerator. Mirroring that model makes the
    local report match the Codecov project percentage instead of the higher raw
    LCOV line percentage.
    """
    hits = 0
    misses = 0
    partials = 0
    for line_no, line_hit_count in line_hits.items():
        branch_hits = branch_hits_by_line.get(line_no, [])
        if line_hit_count <= 0:
            misses += 1
        elif any(branch_hit_count <= 0 for branch_hit_count in branch_hits):
            partials += 1
        else:
            hits += 1

    return CodecovLineCounts(hits=hits, misses=misses, partials=partials)


def _collect_branch_counts(branch_hits_by_line: dict[int, list[int]]) -> CoverageCounts:
    branch_hits = [
        branch_hit_count
        for line_branch_hits in branch_hits_by_line.values()
        for branch_hit_count in line_branch_hits
    ]
    return CoverageCounts(
        hit=sum(1 for branch_hit_count in branch_hits if branch_hit_count > 0),
        found=len(branch_hits),
    )


def _parse_da(line: str) -> tuple[int | None, int]:
    fields = line[3:].split(",")
    if len(fields) < 2:
        return None, 0
    try:
        return int(fields[0]), int(fields[1])
    except ValueError:
        return None, 0


def _parse_brda(line: str) -> tuple[int | None, int]:
    fields = line[5:].split(",")
    if len(fields) != 4:
        return None, 0
    try:
        line_no = int(fields[0])
    except ValueError:
        return None, 0

    value = fields[3]
    if value == "-":
        return line_no, 0
    try:
        return line_no, int(value)
    except ValueError:
        return line_no, 0


def metrics_to_json(
    metrics: LcovMetrics, coverage_target: float, branch_target: float, top_misses: int
) -> dict[str, Any]:
    """Return a JSON-serializable metrics summary."""
    top_files = sorted(metrics.files, key=lambda file: file.missed_branches, reverse=True)
    top_partial_files = sorted(
        metrics.files, key=lambda file: file.codecov_lines.partials, reverse=True
    )
    return {
        "source_files": len(metrics.files),
        "lines": {
            "hit": metrics.lines.hit,
            "found": metrics.lines.found,
            "percent": metrics.lines.percent,
        },
        "codecov_lines": {
            "hits": metrics.codecov_lines.hits,
            "misses": metrics.codecov_lines.misses,
            "partials": metrics.codecov_lines.partials,
            "total": metrics.codecov_lines.total,
            "percent": metrics.codecov_lines.percent,
            "display_percent": metrics.codecov_lines.display_percent,
            "target_percent": coverage_target,
            "hits_needed_for_target": metrics.codecov_lines.hits_needed_for(coverage_target),
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
        "top_codecov_partials": [
            {
                "source_file": file.source_file,
                "partials": file.codecov_lines.partials,
                "hits": file.codecov_lines.hits,
                "misses": file.codecov_lines.misses,
                "total": file.codecov_lines.total,
                "percent": file.codecov_lines.percent,
            }
            for file in top_partial_files
            if file.codecov_lines.partials > 0
        ][:top_misses],
    }


def print_text_summary(
    metrics: LcovMetrics, coverage_target: float, branch_target: float, top_misses: int
) -> None:
    """Print a compact human-readable coverage summary."""
    print("Filtered LCOV coverage metrics:")
    print(f"  source files: {len(metrics.files)}")
    print(
        f"  raw executed lines: {metrics.lines.hit}/{metrics.lines.found} "
        f"({metrics.lines.percent:.2f}%)"
    )
    print(
        f"  codecov lines: {metrics.codecov_lines.hits}/"
        f"{metrics.codecov_lines.total} ({metrics.codecov_lines.percent:.2f}%)"
    )
    print(f"    codecov display: {metrics.codecov_lines.display_percent}%")
    print(
        f"    hits={metrics.codecov_lines.hits} misses={metrics.codecov_lines.misses} "
        f"partials={metrics.codecov_lines.partials}"
    )
    print(
        f"  codecov lines to {coverage_target:g}%: "
        f"{metrics.codecov_lines.hits_needed_for(coverage_target)}"
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

    top_partial_files = sorted(
        metrics.files, key=lambda file: file.codecov_lines.partials, reverse=True
    )
    printed = 0
    for file in top_partial_files:
        if file.codecov_lines.partials <= 0:
            continue
        if printed == 0:
            print("  top codecov partial lines:")
        print(
            f"    {file.codecov_lines.partials:4d} "
            f"{file.codecov_lines.hits:4d}/{file.codecov_lines.total:<4d} {file.source_file}"
        )
        printed += 1
        if printed >= top_misses:
            break

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
        "--coverage-target",
        type=float,
        default=90.0,
        help="Codecov-style line coverage target percentage to compute the shortfall for",
    )
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
        help="Number of source files with the most partial lines and missed branches to print",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    parser.add_argument(
        "--codecov-reference-json",
        type=Path,
        help=(
            "Codecov commit API JSON whose processed file/line universe should be used "
            "instead of every DA record in the LCOV report"
        ),
    )
    args = parser.parse_args()

    try:
        reference_lines = (
            load_codecov_reference_lines(args.codecov_reference_json)
            if args.codecov_reference_json is not None
            else None
        )
        metrics = collect_lcov_metrics(args.report, reference_lines=reference_lines)
    except FileNotFoundError:
        print(f"ERROR: Coverage report not found: {args.report}", file=sys.stderr)
        return 1

    if args.json:
        print(
            json.dumps(
                metrics_to_json(
                    metrics, args.coverage_target, args.branch_target, args.top_misses
                ),
                indent=2,
            )
        )
    else:
        if args.codecov_reference_json is not None:
            print(f"Using Codecov reference lines from {args.codecov_reference_json}")
        print_text_summary(metrics, args.coverage_target, args.branch_target, args.top_misses)
    return 0


if __name__ == "__main__":
    sys.exit(main())
