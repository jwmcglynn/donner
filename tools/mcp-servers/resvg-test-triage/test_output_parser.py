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


@dataclass
class TestResult:
    """Represents a single test result."""
    test_name: str
    status: str  # "PASSED", "FAILED", "SKIPPED"
    pixel_diff: Optional[int] = None
    # Category directory in the new layout (e.g. "text/textPath",
    # "painting/fill"). Replaces the old category_prefix field.
    category: Optional[str] = None


@dataclass
class TestSummary:
    """Summary statistics for a test run."""
    total_tests: int
    passed: int
    failed: int
    skipped: int
    completion_rate: float  # 0.0 to 1.0


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


def parse_test_output(test_output: str) -> tuple[list[TestResult], TestSummary]:
    """
    Parse bazel test output to extract test results.

    Handles the current GoogleTest format used by the resvg test suite:

        [ RUN      ] TextTextPath/ImageComparisonTestFixture.ResvgTest/simple_case
        [  COMPARE ] /.../tests/text/textPath/simple-case.svg [TinySkia]: ...
        [       OK ] TextTextPath/ImageComparisonTestFixture.ResvgTest/simple_case
        [  FAILED  ] TextTextPath/ImageComparisonTestFixture.ResvgTest/x, where GetParam() = /.../tests/text/textPath/x.svg

    The SVG filename (and therefore category) is extracted from the COMPARE
    line or from the ``where GetParam() = <path>`` suffix on the summary
    FAILED line — not from the mangled test ID, which has ``_`` swapped for
    ``/`` / ``-`` / ``=`` and is not round-trippable.

    Args:
        test_output: Complete output from 'bazel test' command

    Returns:
        Tuple of (test_results, summary)
    """
    results = []
    current_status: Optional[str] = None
    current_diff: Optional[int] = None
    current_svg_path: Optional[str] = None
    current_test_id: Optional[str] = None
    seen: set[tuple[str, str]] = set()  # (key, status) to dedupe summary lines

    def flush() -> None:
        """Emit a TestResult for the in-flight test, then reset state.

        Prefers the SVG path (rich: filename + category) but falls back to
        the GoogleTest test ID (e.g. ``Suite/Fixture.ResvgTest/param``) when
        no path was emitted — common for tests that hit ``GTEST_SKIP()``
        before the fixture prints its ``[ COMPARE ]`` line. Without the
        fallback, every runtime-skipped test was being silently dropped.
        """
        nonlocal current_status, current_diff, current_svg_path, current_test_id
        if current_status:
            key: Optional[str] = None
            test_name: Optional[str] = None
            category: Optional[str] = None
            if current_svg_path:
                test_name = current_svg_path.rsplit("/", 1)[-1]
                category = extract_category_from_path(current_svg_path)
                key = current_svg_path
            elif current_test_id:
                # Best-effort recovery — we can't reverse the GoogleTest
                # name sanitization to recover the SVG filename, but we
                # can keep the record so callers see accurate skip counts
                # and the test ID for triage cross-reference.
                test_name = current_test_id
                key = current_test_id
            if key is not None and (key, current_status) not in seen:
                seen.add((key, current_status))
                results.append(TestResult(
                    test_name=test_name,
                    status=current_status,
                    pixel_diff=current_diff,
                    category=category,
                ))
        current_status = None
        current_diff = None
        current_svg_path = None
        current_test_id = None

    for line in test_output.split('\n'):
        # [ RUN ] Suite/Fixture.TestName/param_id — starts a new test.
        run_match = re.search(r'\[\s*RUN\s*\].*ResvgTest/(\S+)', line)
        if run_match:
            flush()
            current_test_id = run_match.group(1)
            continue

        # [  COMPARE ] /absolute/path/to/foo.svg [Backend]: ...
        # This is where we learn the actual SVG filename + category.
        compare_match = re.search(
            r'\[\s*COMPARE\s*\]\s+(\S+\.svg)', line)
        if compare_match:
            current_svg_path = compare_match.group(1)
            # Pixel diff, if present on the same line.
            diff_match = re.search(r'(\d+)\s+pixels?\s+differ', line)
            if diff_match:
                current_diff = int(diff_match.group(1))
            continue

        # Result lines: [       OK ], [  FAILED  ], [  SKIPPED ].
        # Both FAILED and SKIPPED summary lines may carry the SVG path
        # via ``where GetParam() = /x``; check for it before flushing
        # so callers see the correct path even when the in-test
        # [ COMPARE ] line was never emitted (e.g. GTEST_SKIP() fires
        # before the fixture body runs).
        if re.search(r'\[\s*OK\s*\]', line):
            current_status = "PASSED"
            flush()
            continue
        if re.search(r'\[\s*FAILED\s*\]', line):
            where_match = re.search(
                r'where\s+GetParam\(\)\s*=\s*(\S+\.svg)', line)
            if where_match:
                current_svg_path = where_match.group(1)
            current_status = "FAILED"
            flush()
            continue
        if re.search(r'\[\s*SKIPPED\s*\]', line):
            where_match = re.search(
                r'where\s+GetParam\(\)\s*=\s*(\S+\.svg)', line)
            if where_match:
                current_svg_path = where_match.group(1)
            # Pull the test ID off the SKIPPED line if we don't already
            # have one — the summary block at end-of-output emits one
            # SKIPPED line per test without a preceding [ RUN ].
            if current_test_id is None:
                id_match = re.search(r'ResvgTest/(\S+)', line)
                if id_match:
                    current_test_id = id_match.group(1)
            current_status = "SKIPPED"
            flush()
            continue
        # GTest also emits a bare "Skipped" message on disabled tests:
        if current_test_id and "DISABLED_" in line:
            current_status = "SKIPPED"
            flush()

    # Any trailing in-flight entry.
    flush()

    total = len(results)
    passed = sum(1 for r in results if r.status == "PASSED")
    failed = sum(1 for r in results if r.status == "FAILED")
    skipped = sum(1 for r in results if r.status == "SKIPPED")
    completion_rate = passed / total if total > 0 else 0.0

    summary = TestSummary(
        total_tests=total,
        passed=passed,
        failed=failed,
        skipped=skipped,
        completion_rate=completion_rate,
    )
    return results, summary


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


def group_by_category(results: list[TestResult]) -> dict[str, CategoryReport]:
    """
    Group test results by category directory (e.g. ``text/textPath``).

    Args:
        results: List of test results

    Returns:
        Dictionary mapping category directory -> CategoryReport
    """
    categories = {}

    for result in results:
        category = result.category or "other"

        if category not in categories:
            categories[category] = {
                "tests": [],
                "passed": 0,
                "failed": 0,
                "skipped": 0,
            }

        categories[category]["tests"].append(result)

        if result.status == "PASSED":
            categories[category]["passed"] += 1
        elif result.status == "FAILED":
            categories[category]["failed"] += 1
        elif result.status == "SKIPPED":
            categories[category]["skipped"] += 1

    # Convert to CategoryReport objects
    reports = {}
    for category, data in categories.items():
        total = len(data["tests"])
        passing = data["passed"]
        completion_rate = passing / total if total > 0 else 0.0

        reports[category] = CategoryReport(
            category=category,
            total_tests=total,
            passing=passing,
            failing=data["failed"],
            skipped=data["skipped"],
            completion_rate=completion_rate,
            test_results=data["tests"]
        )

    return reports


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
