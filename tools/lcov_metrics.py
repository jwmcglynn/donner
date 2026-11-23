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
from typing import Any, Callable


@dataclass(frozen=True)
class CoverageCounts:
    """Hit/found coverage counts."""

    hit: int = 0
    found: int = 0

    @property
    def percent(self) -> float | None:
        """Return hit/found as a percentage, or None when nothing was counted."""
        if self.found == 0:
            return None
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
    def percent(self) -> float | None:
        """Return Codecov-style line coverage percentage, or None without coverable lines."""
        if self.total == 0:
            return None
        return self.hits / self.total * 100.0

    @property
    def display_percent(self) -> int | None:
        """Return the whole-number percentage shown by Codecov's project UI."""
        percent = self.percent
        return None if percent is None else int(percent + 0.5)

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


class CoverageInputError(ValueError):
    """An input that cannot be measured; the message is the user-facing diagnostic."""


_Reject = Callable[[str], CoverageInputError]

_QUOTE_LIMIT = 120

_REPORT_ENDPOINT_HINT = (
    "expected a Codecov coverage report response with a 'files' list, as returned by "
    "https://api.codecov.io/api/v2/github/<owner>/repos/<repo>/report/?sha=<sha>"
)


def load_codecov_reference_lines(path: Path) -> dict[str, set[int]]:
    """Load the file/line universe from a Codecov coverage report JSON response.

    Accepts the `/report/?sha=<sha>` response, whose top level carries `files`, and the
    `/commits/<sha>/` response, which nests the same report under `report`. Each file
    entry has a `name` and `line_coverage` pairs of `[line_number, coverage]`.

    Raises:
        CoverageInputError: The file is missing, is not a Codecov report, has a
            malformed entry, or describes no coverable lines. Treating any of these
            as an empty universe would discard every local record.
    """

    def reject(problem: str) -> CoverageInputError:
        return CoverageInputError(f"Codecov reference {path} {problem}")

    reference_lines: dict[str, set[int]] = {}
    for file_index, file_entry in enumerate(_reference_files(_read_reference_json(path), reject)):
        location = f"files[{file_index}]"
        source_file, lines = _reference_file_lines(file_entry, location, reject)
        if source_file in reference_lines:
            raise reject(f"lists {_quote(source_file)} twice (again at {location})")
        reference_lines[source_file] = lines

    if not any(reference_lines.values()):
        raise reject(f"has no coverable lines in its {len(reference_lines)} files")
    return reference_lines


def _read_reference_json(path: Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise CoverageInputError(f"Codecov reference JSON not found: {path}") from None
    except (OSError, UnicodeDecodeError) as error:
        raise CoverageInputError(f"Cannot read Codecov reference JSON {path}: {error}") from None

    try:
        return json.loads(text)
    except (ValueError, RecursionError) as error:
        # ValueError covers JSONDecodeError and integers past the int-to-str digit limit;
        # RecursionError covers nesting deeper than the decoder allows.
        raise CoverageInputError(
            f"Codecov reference {path} is not valid JSON ({error}); {_REPORT_ENDPOINT_HINT}"
        ) from None


def _reference_files(data: Any, reject: _Reject) -> list[Any]:
    """Return the non-empty `files` list of a report or commit response."""
    if not isinstance(data, dict):
        raise reject(f"is not a JSON object; {_REPORT_ENDPOINT_HINT}")
    if isinstance(data.get("detail"), str):
        raise reject(f"is a Codecov error response: {_quote(data['detail'])}")

    report = data["report"] if "report" in data else data
    files = report.get("files") if isinstance(report, dict) else None
    if not isinstance(files, list):
        raise reject(f"has no 'files' list; {_REPORT_ENDPOINT_HINT}")
    if not files:
        raise reject("lists no files; Codecov may not have processed a report for this commit")
    return files


def _reference_file_lines(file_entry: Any, location: str, reject: _Reject) -> tuple[str, set[int]]:
    """Return the name and line numbers of one `files` entry."""
    source_file = file_entry.get("name") if isinstance(file_entry, dict) else None
    if not isinstance(source_file, str) or not source_file:
        raise reject(f"has a malformed entry at {location}: expected an object with a 'name'")

    line_coverage = file_entry.get("line_coverage")
    if not isinstance(line_coverage, list):
        raise reject(f"has no 'line_coverage' list for {_quote(source_file)} at {location}")
    return source_file, {
        _reference_line_number(line_entry, f"{location}.line_coverage[{line_index}]", reject)
        for line_index, line_entry in enumerate(line_coverage)
    }


def _reference_line_number(line_entry: Any, location: str, reject: _Reject) -> int:
    """Return the line number from a `[line_number, coverage]` pair."""
    if isinstance(line_entry, list) and len(line_entry) >= 2:
        line_no = line_entry[0]
        if isinstance(line_no, int) and not isinstance(line_no, bool) and line_no > 0:
            return line_no
    raise reject(
        f"has a malformed entry at {location}: expected [line_number, coverage] with a "
        f"positive integer line number, got {_quote(line_entry)}"
    )


def _quote(value: Any) -> str:
    """Return untrusted JSON as a bounded, escaped literal safe to print in a diagnostic.

    JSON escaping keeps control characters such as ESC and newline out of terminal and CI
    log output, where they could forge escape sequences or workflow commands.
    """
    try:
        text = json.dumps(value)
    except (TypeError, ValueError, RecursionError):
        return f"<{type(value).__name__}>"
    return text if len(text) <= _QUOTE_LIMIT else text[: _QUOTE_LIMIT - 3] + "..."


def require_line_universe(metrics: LcovMetrics, report: Path) -> None:
    """Reject a report with no local line records instead of reporting a percentage.

    With a reference, the universe comes from the reference, so an empty LCOV report would
    otherwise measure as every reference line missed.
    """
    if count_line_records(report) == 0 or metrics.codecov_lines.total == 0:
        raise CoverageInputError(
            f"Coverage report {report} has no executable line (DA) records; "
            "there is no line universe to measure"
        )


def count_line_records(path: Path) -> int:
    """Return the parsed `DA` records in an LCOV report, before any reference filtering."""
    with path.open(encoding="utf-8", errors="replace") as lcov_file:
        return sum(
            1
            for raw_line in lcov_file
            if raw_line.startswith("DA:") and _parse_da(raw_line.rstrip("\n"))[0] is not None
        )


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
            "Codecov coverage report API JSON (/report/?sha=<sha>) whose processed "
            "file/line universe should be used instead of every DA record in the LCOV report"
        ),
    )
    args = parser.parse_args()

    try:
        reference_lines = (
            load_codecov_reference_lines(args.codecov_reference_json)
            if args.codecov_reference_json is not None
            else None
        )
        try:
            metrics = collect_lcov_metrics(args.report, reference_lines=reference_lines)
        except FileNotFoundError:
            raise CoverageInputError(f"Coverage report not found: {args.report}") from None
        require_line_universe(metrics, args.report)
    except CoverageInputError as error:
        print(f"ERROR: {error}", file=sys.stderr)
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
