"""
Parser for bazel test output to extract test results and statistics.

This module provides utilities for parsing test output from bazel test runs,
extracting pass/fail/skip counts, and organizing results by category.

Post-upgrade (linebender/resvg-test-suite, 2024+), tests live under
tests/<category>/<feature>/<name>.svg rather than the old flat svg/a-*.svg /
svg/e-*.svg layout. The "category" is therefore the parent directory path
(e.g. ``text/textPath``, ``painting/fill``), not a filename prefix.
"""

import re
from dataclasses import dataclass
from typing import Optional

from triage_evidence import bounded_text


@dataclass
class TestResult:
    """Represents a single test result."""
    test_name: str
    status: str  # "PASSED", "FAILED", "SKIPPED", "INCOMPLETE"
    pixel_diff: Optional[int] = None
    # Category directory in the new layout (e.g. "text/textPath",
    # "painting/fill"). Replaces the old category_prefix field.
    category: Optional[str] = None
    test_id: Optional[str] = None
    svg_path: Optional[str] = None
    raw_output: str = ""


@dataclass
class TestSummary:
    """Summary statistics for a test run."""
    total_tests: int
    passed: int
    failed: int
    skipped: int
    completion_rate: float  # Observed passing fraction, not proof of a complete run.
    incomplete: int = 0
    run_complete: bool = False


@dataclass
class CategoryReport:
    """Test results grouped by category."""
    category: str
    total_tests: int
    passing: int
    failing: int
    skipped: int
    completion_rate: float
    test_results: list[TestResult]
    incomplete: int = 0


def extract_category_from_path(svg_path: str) -> Optional[str]:
    """
    Extract the category directory from an SVG runfile path.

    The post-upgrade layout places every test at
    ``<runfiles root>/tests/<category>/<feature>/<name>.svg``. The category
    is the relative directory portion (everything between ``tests/`` and the
    final filename) — e.g. ``text/textPath`` or ``painting/fill``.

    Examples:
        ".../resvg-test-suite/tests/painting/fill/rgb-int-int-int.svg"
            -> "painting/fill"
        ".../resvg-test-suite/tests/text/textPath/simple-case.svg"
            -> "text/textPath"
        "some/unrelated/path.svg"
            -> None

    Args:
        svg_path: Absolute or relative path to the .svg test file.

    Returns:
        Category directory relative to ``tests/`` or None if the path does
        not live under a ``tests/`` segment.
    """
    if not svg_path:
        return None
    # Find the last occurrence of "/tests/" (or leading "tests/") and take
    # everything after it up to the final filename.
    marker = "/tests/"
    idx = svg_path.rfind(marker)
    if idx < 0:
        if svg_path.startswith("tests/"):
            rest = svg_path[len("tests/"):]
        else:
            return None
    else:
        rest = svg_path[idx + len(marker):]
    if "/" not in rest:
        return None
    category, _, _ = rest.rpartition("/")
    return category or None


def _result_record(test_id, status, path, diff, raw_output) -> TestResult:
    return TestResult(
        test_name=path.rsplit("/", 1)[-1] if path else test_id,
        status=status,
        pixel_diff=diff,
        category=extract_category_from_path(path) if path else None,
        test_id=test_id,
        svg_path=path,
        raw_output=raw_output,
    )


def _enrich_summary(result: TestResult, path: Optional[str], line: str) -> None:
    if path and result.svg_path is None:
        result.svg_path = path
        result.test_name = path.rsplit("/", 1)[-1]
        result.category = extract_category_from_path(path)
    result.raw_output += "\n" + line


def _comparator_record(line: str) -> Optional[tuple[str, str]]:
    """Separate the fixed record header and final backend marker from the path."""
    header = re.match(r"^[ \t]*\[[ \t]*COMPARE[ \t]*\][ \t]+", line)
    if not header:
        return None
    contents = line[header.end():].rstrip(" \t")
    path, marker, record = contents.rpartition(" [")
    backend, separator, payload = record.partition("]:")
    if (marker and separator and path.endswith(".svg") and backend
            and not any(char in backend for char in "[]")):
        return path, payload.lstrip(" \t")
    if not marker and contents.endswith(".svg"):
        return contents, ""
    return None


class _OutputParser:
    """Keep the active test separate from completed records and summary repeats."""

    def __init__(self):
        self.results: list[TestResult] = []
        self.latest: dict[tuple[str, str], TestResult] = {}
        self.terminal_count: Optional[int] = None
        self.reset_active()

    def reset_active(self) -> None:
        self.current_id: Optional[str] = None
        self.pending: list[str] = []
        self.current_path: Optional[str] = None
        self.current_diff: Optional[int] = None

    def retain_incomplete(self) -> None:
        if self.current_id is not None:
            self.results.append(_result_record(
                self.current_id, "INCOMPLETE", self.current_path, self.current_diff,
                "\n".join(self.pending),
            ))

    def start(self, test_id: str, line: str) -> None:
        self.retain_incomplete()
        self.reset_active()
        self.terminal_count = None
        self.current_id = test_id
        self.pending = [line]

    def observe(self, line: str) -> None:
        self.pending.append(line)
        compare = _comparator_record(line)
        if compare is None:
            return
        self.current_path, payload = compare
        metric = re.match(
            r"(?:(?:FAIL|PASS)[ \t]+\()?([0-9]{1,10})(?![0-9])"
            r"[ \t]+pixels?[ \t]+differ(?:ent)?\b", payload
        )
        self.current_diff = int(metric.group(1)) if metric else None

    def finish_test(self, test_id: str, status: str, line: str) -> None:
        is_active = self.current_id == test_id
        path = self.current_path if is_active else None
        where = re.search(r"where\s+GetParam\(\)\s*=\s*(.+?\.svg)(?:$|[,\s])", line)
        if where:
            path = where.group(1)
        previous = self.latest.get((test_id, status))
        if previous is not None and not is_active:
            _enrich_summary(previous, path, line)
            return
        result = _result_record(
            test_id, status, path, self.current_diff if is_active else None,
            "\n".join(self.pending) if is_active else line,
        )
        self.results.append(result)
        self.latest[(test_id, status)] = result
        self.reset_active()

    def consume(self, line: str) -> None:
        run = re.search(r"\[\s*RUN\s*\]\s+(\S+)", line)
        if run:
            self.start(run.group(1), line)
            return
        footer = re.search(
            r"\[={10}\]\s+([0-9]{1,10})(?![0-9])\s+tests? from .+ ran\.", line
        )
        if footer:
            self.terminal_count = int(footer.group(1))
        if self.current_id:
            self.observe(line)
        terminal = re.search(r"\[\s*(OK|FAILED|SKIPPED)\s*\]\s+(\S+)", line)
        if terminal and "." in terminal.group(2):
            statuses = {"OK": "PASSED", "FAILED": "FAILED", "SKIPPED": "SKIPPED"}
            self.finish_test(terminal.group(2).rstrip(","), statuses[terminal.group(1)], line)

    def summary(self) -> TestSummary:
        total = len(self.results)
        passed = sum(item.status == "PASSED" for item in self.results)
        failed = sum(item.status == "FAILED" for item in self.results)
        skipped = sum(item.status == "SKIPPED" for item in self.results)
        incomplete = sum(item.status == "INCOMPLETE" for item in self.results)
        return TestSummary(
            total, passed, failed, skipped, passed / total if total else 0.0,
            incomplete=incomplete,
            run_complete=self.terminal_count == total and incomplete == 0,
        )


def parse_test_output(test_output: str) -> tuple[list[TestResult], TestSummary]:
    """Retain quiet failures, incomplete cases, comparator metrics and raw output.

    Full test IDs identify records. Summary repeats enrich existing records; SVG
    paths come only from COMPARE or GetParam, never from reversing sanitized IDs.
    """
    bounded_text(test_output)
    parser = _OutputParser()
    for line in test_output.splitlines():
        parser.consume(line)
    parser.retain_incomplete()
    return parser.results, parser.summary()


def parse_skip_file(skip_file_content: str) -> dict[str, str]:
    """
    Parse resvg_test_suite.cc skip entries to extract test -> reason mapping.

    After the M1 upgrade, reasons are carried as string arguments to the
    Skip factory: ``{"foo.svg", Params::Skip("Not impl: textPath")}``. The
    pre-upgrade trailing-comment form is still recognized as a fallback so
    this module can also read historic skip files.

    Args:
        skip_file_content: Content of resvg_test_suite.cc

    Returns:
        Dictionary mapping test_name -> skip_reason
    """
    skip_map: dict[str, str] = {}

    # New form: Skip("reason") with optional trailing comment.
    # The reason string is the authoritative source.
    new_form = re.compile(
        r'\{\s*"(?P<name>[^"]+\.svg)"\s*,\s*'
        r'Params::Skip\(\s*"(?P<reason>[^"]*)"\s*\)'
    )
    for match in new_form.finditer(skip_file_content):
        skip_map[match.group("name")] = match.group("reason").strip()

    # Legacy form: Skip() with a trailing "// comment" on the same line.
    legacy_form = re.compile(
        r'\{\s*"(?P<name>[^"]+\.svg)"\s*,\s*Params::Skip\(\s*\)\s*\}\s*,'
        r'\s*//\s*(?P<reason>.+)'
    )
    for match in legacy_form.finditer(skip_file_content):
        # Only fill in if the newer form didn't already cover this test.
        skip_map.setdefault(match.group("name"), match.group("reason").strip())

    return skip_map


def _category_report(category: str, tests: list[TestResult]) -> CategoryReport:
    total = len(tests)
    passing = sum(item.status == "PASSED" for item in tests)
    return CategoryReport(
        category=category,
        total_tests=total,
        passing=passing,
        failing=sum(item.status == "FAILED" for item in tests),
        skipped=sum(item.status == "SKIPPED" for item in tests),
        completion_rate=passing / total if total else 0.0,
        test_results=tests,
        incomplete=sum(item.status == "INCOMPLETE" for item in tests),
    )


def group_by_category(results: list[TestResult]) -> dict[str, CategoryReport]:
    """Group observed results by category, preserving incomplete cases in totals."""
    categories: dict[str, list[TestResult]] = {}
    for result in results:
        categories.setdefault(result.category or "other", []).append(result)
    return {name: _category_report(name, tests) for name, tests in categories.items()}


def identify_missing_features(skip_map: dict[str, str], category: Optional[str] = None) -> dict[str, list[str]]:
    """
    Identify missing features and the tests they affect.

    Since the post-upgrade skip file keys the map by bare SVG filename
    (e.g. ``simple-case.svg``) without the category directory, the
    ``category`` filter is advisory: callers should scope the skip_map
    themselves before calling if they want strict category filtering.
    A passed ``category`` is therefore ignored here — preserved as a
    parameter for backwards compatibility.

    Args:
        skip_map: Dictionary from parse_skip_file()
        category: Advisory; no-op in the directory-based layout.

    Returns:
        Dictionary mapping feature -> list of affected tests
    """
    del category  # parameter preserved for backwards compatibility
    features = {}

    for test_name, reason in skip_map.items():
        # Extract feature from skip reason.
        # Recognized patterns:
        #   "Not impl: dx attribute"          -> "dx attribute"
        #   "Not impl: `letter-spacing`"      -> "letter-spacing"
        #   "Not impl: <textPath>"            -> "textPath"
        #   "M1 upgrade: needs triage"        -> no feature extracted
        feature_match = re.search(r'Not impl:\s*[`<]?([^>`]+?)[>`]?\s*$', reason)
        if feature_match:
            feature = feature_match.group(1).strip()
            if feature not in features:
                features[feature] = []
            features[feature].append(test_name)

    return features


def get_next_priority_feature(feature_map: dict[str, list[str]]) -> Optional[tuple[str, int]]:
    """
    Determine the next priority feature to implement based on test impact.

    Args:
        feature_map: Dictionary from identify_missing_features()

    Returns:
        Tuple of (feature_name, test_count) or None if no features
    """
    if not feature_map:
        return None

    # Sort by number of affected tests (descending)
    sorted_features = sorted(
        feature_map.items(),
        key=lambda x: len(x[1]),
        reverse=True
    )

    feature, tests = sorted_features[0]
    return (feature, len(tests))
