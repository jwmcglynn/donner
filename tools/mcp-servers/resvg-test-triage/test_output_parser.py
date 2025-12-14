"""
Parser for bazel test output to extract test results and statistics.

This module provides utilities for parsing test output from bazel test runs,
extracting pass/fail/skip counts, and organizing results by category.
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
    category_prefix: Optional[str] = None  # e.g., "e-text", "a-transform"


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


def extract_test_prefix(test_name: str) -> Optional[str]:
    """
    Extract the category prefix from a test name.

    Examples:
        "e-text-002.svg" -> "e-text"
        "a-transform-001.svg" -> "a-transform"
        "custom-test.svg" -> None

    Args:
        test_name: Test file name

    Returns:
        Category prefix or None if no standard prefix found
    """
    match = re.match(r'^([a-z]+-[a-z]+)-\d+\.svg$', test_name)
    if match:
        return match.group(1)
    return None


def parse_test_output(test_output: str) -> tuple[list[TestResult], TestSummary]:
    """
    Parse bazel test output to extract test results.

    Args:
        test_output: Complete output from 'bazel test' command

    Returns:
        Tuple of (test_results, summary)
    """
    results = []
    current_test = None
    current_status = None
    current_diff = None

    for line in test_output.split('\n'):
        # Match test start: [ RUN      ] ResvgTest/e_text_002
        run_match = re.search(r'\[\s*RUN\s*\].*ResvgTest/(\w+)', line)
        if run_match:
            current_test = run_match.group(1).replace('_', '-') + '.svg'
            current_status = None
            current_diff = None

        # Match test result: [       OK ] or [  FAILED  ]
        elif re.search(r'\[\s*OK\s*\]', line) and current_test:
            current_status = "PASSED"
        elif re.search(r'\[\s*FAILED\s*\]', line) and current_test:
            current_status = "FAILED"

        # Match pixel diff: "COMPARE ] FAIL: 1234 pixels differ"
        elif "[  COMPARE ]" in line and "FAIL" in line:
            diff_match = re.search(r'(\d+)\s+pixels?\s+differ', line)
            if diff_match:
                current_diff = int(diff_match.group(1))

        # Match skipped test (from test output or skip list)
        elif "SKIPPED" in line and current_test:
            current_status = "SKIPPED"

        # When test completes, save result
        if current_test and current_status:
            prefix = extract_test_prefix(current_test)
            result = TestResult(
                test_name=current_test,
                status=current_status,
                pixel_diff=current_diff,
                category_prefix=prefix
            )
            results.append(result)
            current_test = None
            current_status = None
            current_diff = None

    # Calculate summary
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
        completion_rate=completion_rate
    )

    return results, summary


def parse_skip_file(skip_file_content: str) -> dict[str, str]:
    """
    Parse resvg_test_suite.cc skip entries to extract test -> reason mapping.

    Args:
        skip_file_content: Content of resvg_test_suite.cc

    Returns:
        Dictionary mapping test_name -> skip_reason
    """
    skip_map = {}

    # Match skip entries like: {"e-text-002.svg", Params::Skip()},  // Not impl: dx attribute
    pattern = r'\{\"([^\"]+)\",\s*Params::Skip\(\)\},\s*//\s*(.+)'

    for match in re.finditer(pattern, skip_file_content):
        test_name = match.group(1)
        reason = match.group(2).strip()
        skip_map[test_name] = reason

    return skip_map


def group_by_category(results: list[TestResult]) -> dict[str, CategoryReport]:
    """
    Group test results by category prefix.

    Args:
        results: List of test results

    Returns:
        Dictionary mapping category -> CategoryReport
    """
    categories = {}

    for result in results:
        category = result.category_prefix or "other"

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

    Args:
        skip_map: Dictionary from parse_skip_file()
        category: Optional category filter (e.g., "e-text")

    Returns:
        Dictionary mapping feature -> list of affected tests
    """
    features = {}

    for test_name, reason in skip_map.items():
        # Filter by category if specified
        if category:
            prefix = extract_test_prefix(test_name)
            if prefix != category:
                continue

        # Extract feature from skip reason
        # "Not impl: dx attribute" -> "dx attribute"
        # "Not impl: `letter-spacing`" -> "letter-spacing"
        feature_match = re.search(r'Not impl:\s*[`<]?([^>`]+)[>`]?', reason)
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
