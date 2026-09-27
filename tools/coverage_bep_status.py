"""Classify why a `bazel coverage` run produced no coverage report.

`tools/coverage.sh` fails closed when the report is missing, because a silently
empty report would report full coverage of nothing. That guard is right for the
usual causes (a build error, a crashed test, a cancelled run), but it also fires
on a legitimate case: every selected target is `target_compatible_with` a
platform this lane does not run on, so Bazel skips them all and there is nothing
to measure. A pull request that only touches such a target hits this on every
run and can never go green.

This module reads the build event protocol stream and separates those cases, so
the shell script keeps failing closed except when the stream positively shows
that skipping accounted for every target.

Reads a BEP JSON-lines file; writes one JSON document to stdout:

    {"status": "all_skipped", "skipped": ["//pkg:target"], "produced": []}

`status` is one of:

  all_skipped  At least one target was skipped for incompatibility and NO
               target produced a result. Nothing was measurable here.
  has_results  At least one target produced a result, so a missing report is a
               real failure.
  unknown      The stream is absent, empty, or unparseable. Callers must treat
               this as a failure: absence of evidence is not evidence that
               skipping happened.
"""

import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ALL_SKIPPED = "all_skipped"
HAS_RESULTS = "has_results"
UNKNOWN = "unknown"
_SAFE_LABEL = re.compile(r"(?:@@?[A-Za-z0-9_.+~%-]+)?//[A-Za-z0-9_./:+*=-]+\Z")
_FAILED_TEST_STATUSES = {
    "FAILED", "TIMEOUT", "INCOMPLETE", "REMOTE_FAILURE",
    "FAILED_TO_BUILD", "TOOL_HALTED_BEFORE_TESTING",
}
_FAILED_ABORT_REASONS = {"LOADING_FAILURE", "ANALYSIS_FAILURE"}
_MAX_FAILURE_LABELS = 20
_GTEST_XML_BY_LABEL = {
    "//donner/gpu/baseline:baseline_pixels_tests":
        Path("donner/gpu/baseline/baseline_pixels_tests/test.xml"),
    "//donner/gpu/vulkan/tests:vulkan_color_matrix_tests":
        Path("donner/gpu/vulkan/tests/vulkan_color_matrix_tests/test.xml"),
    "//donner/svg/renderer/tests:renderer_geode_golden_tests":
        Path("donner/svg/renderer/tests/renderer_geode_golden_tests/test.xml"),
}
_SAFE_GTEST_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:/[A-Za-z0-9_]+)*\Z")
_MAX_GTEST_XML_BYTES = 1024 * 1024
_MAX_GTEST_CASES_SCANNED = 1000
_MAX_GTEST_CASES_EMITTED = 30


def classify(lines):
    """Classify an iterable of BEP JSON-lines strings.

    A target that Bazel skips for incompatibility appears as an `aborted` event
    with reason SKIPPED. A target that actually ran appears as a `completed`
    event carrying a success field, or as a test result. Malformed lines are
    ignored rather than fatal: the stream is written incrementally and a run
    killed mid-write can leave a partial final line.
    """
    skipped = []
    produced = []
    saw_any = False

    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict):
            continue
        saw_any = True

        label = _label(event.get("id"))

        aborted = event.get("aborted")
        if isinstance(aborted, dict) and aborted.get("reason") == "SKIPPED":
            if label:
                skipped.append(label)
            continue

        completed = event.get("completed")
        if isinstance(completed, dict) and completed.get("success") and label:
            produced.append(label)
            continue

        if "testResult" in event and label:
            produced.append(label)

    if not saw_any:
        return {"status": UNKNOWN, "skipped": [], "produced": []}
    if produced:
        return {"status": HAS_RESULTS, "skipped": skipped, "produced": produced}
    if skipped:
        return {"status": ALL_SKIPPED, "skipped": skipped, "produced": []}
    return {"status": UNKNOWN, "skipped": [], "produced": []}


def _label(event_id):
    """Pull a target label out of a BEP event id, whichever shape it uses."""
    if not isinstance(event_id, dict):
        return None
    for key in (
        "targetCompleted", "targetConfigured", "testResult", "testSummary",
        "targetSummary", "configuredLabel", "unconfiguredLabel",
    ):
        holder = event_id.get(key)
        if isinstance(holder, dict) and holder.get("label"):
            return holder["label"]
    return None


def _safe_label(label):
    """Only expose bounded Bazel labels, never BEP paths or arbitrary text."""
    return isinstance(label, str) and len(label) <= 512 and bool(_SAFE_LABEL.fullmatch(label))


def _final_test_status(event):
    for name, field in (("testSummary", "overallStatus"),
                        ("targetSummary", "overallTestStatus")):
        payload = event.get(name)
        status = payload.get(field) if isinstance(payload, dict) else None
        if isinstance(status, str):
            return status
    return None


def _failed_test_attempt(event):
    payload = event.get("testResult")
    status = payload.get("status") if isinstance(payload, dict) else None
    return isinstance(status, str) and status in _FAILED_TEST_STATUSES


def _failed_build(event):
    completed = event.get("completed")
    aborted = event.get("aborted")
    reason = aborted.get("reason") if isinstance(aborted, dict) else None
    return (
        isinstance(completed, dict) and completed.get("success") is False
    ) or (
        isinstance(reason, str) and reason in _FAILED_ABORT_REASONS
    )


def failure_summary(lines):
    """Extract path-free failed test and build labels from a BEP stream."""
    final_statuses = {}
    failed_attempts = set()
    builds = set()
    for line in lines:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict):
            continue
        label = _label(event.get("id"))
        if not _safe_label(label):
            continue
        final_status = _final_test_status(event)
        if final_status is not None:
            final_statuses[label] = final_status
        if _failed_test_attempt(event):
            failed_attempts.add(label)
        if _failed_build(event):
            builds.add(label)
    tests = {label for label, status in final_statuses.items()
             if status in _FAILED_TEST_STATUSES}
    tests.update(label for label in failed_attempts if label not in final_statuses)
    sorted_tests = sorted(tests)
    sorted_builds = sorted(builds)
    return {
        "failedTests": sorted_tests[:_MAX_FAILURE_LABELS],
        "failedBuilds": sorted_builds[:_MAX_FAILURE_LABELS],
        "omittedTests": max(0, len(sorted_tests) - _MAX_FAILURE_LABELS),
        "omittedBuilds": max(0, len(sorted_builds) - _MAX_FAILURE_LABELS),
    }


def _gtest_cases_for_target(label, testlogs_root, declared_xml, output_cap):
    """Read only the fixed Bazel test.xml path for an allowlisted failed target."""
    result = {
        "target": label,
        "status": "unavailable",
        "reason": "missing_xml",
        "failedCases": [],
        "omittedCases": 0,
    }
    if not declared_xml:
        result["reason"] = "undeclared_xml"
        return result
    try:
        root_path = Path(testlogs_root).resolve(strict=True)
        if not root_path.is_dir():
            return result
        xml_path = root_path
        for component in _GTEST_XML_BY_LABEL[label].parts:
            xml_path /= component
            if xml_path.is_symlink():
                result["reason"] = "symlink_xml"
                return result
        if not xml_path.resolve(strict=True).is_relative_to(root_path):
            result["reason"] = "path_escape"
            return result
    except (OSError, RuntimeError):
        return result
    try:
        with open(xml_path, "rb") as stream:
            document = stream.read(_MAX_GTEST_XML_BYTES + 1)
    except OSError:
        return result
    if len(document) > _MAX_GTEST_XML_BYTES:
        result["reason"] = "oversize_xml"
        return result
    if b"<!DOCTYPE" in document.upper() or b"<!ENTITY" in document.upper():
        result["reason"] = "invalid_xml"
        return result
    try:
        root = ET.fromstring(document)
        reported_failures = int(root.attrib["failures"]) + int(root.attrib.get("errors", "0"))
    except (ET.ParseError, KeyError, ValueError):
        result["reason"] = "invalid_xml"
        return result
    if (root.tag not in ("testsuites", "testsuite") or reported_failures < 1
            or reported_failures > _MAX_GTEST_CASES_SCANNED):
        result["reason"] = "invalid_xml"
        return result

    failed_cases = []
    for index, case in enumerate(root.iter("testcase")):
        if index >= _MAX_GTEST_CASES_SCANNED:
            result["reason"] = "too_many_cases"
            return result
        if case.find("failure") is None and case.find("error") is None:
            continue
        suite = case.get("classname")
        name = case.get("name")
        if (not isinstance(suite, str) or not isinstance(name, str)
                or len(suite) > 80 or len(name) > 80
                or not _SAFE_GTEST_NAME.fullmatch(suite)
                or not _SAFE_GTEST_NAME.fullmatch(name)):
            result["reason"] = "unsafe_case_name"
            return result
        failed_cases.append(f"{suite}.{name}")
    if not failed_cases or len(set(failed_cases)) != reported_failures:
        result["reason"] = "incomplete_xml"
        return result

    failed_cases = sorted(set(failed_cases))
    return {
        "target": label,
        "status": "cases_found",
        "failedCases": failed_cases[:output_cap],
        "failedCaseCount": len(failed_cases),
        "omittedCases": max(0, len(failed_cases) - output_cap),
    }


def allowlisted_failure_cases(lines, testlogs_root):
    """Emit bounded case names for three named GPU tests, never raw XML text."""
    final_statuses = {}
    failed_attempts = set()
    declared_xml = set()
    for line in lines:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict):
            continue
        label = _label(event.get("id"))
        if label not in _GTEST_XML_BY_LABEL:
            continue
        status = _final_test_status(event)
        if status is not None:
            final_statuses[label] = status
        if _failed_test_attempt(event):
            failed_attempts.add(label)
        attempt = event.get("testResult")
        if isinstance(attempt, dict):
            outputs = attempt.get("testActionOutput")
            if isinstance(outputs, list) and any(
                isinstance(output, dict) and output.get("name") == "test.xml"
                for output in outputs):
                declared_xml.add(label)
    failed_labels = sorted(
        label for label in _GTEST_XML_BY_LABEL
        if final_statuses.get(label) in _FAILED_TEST_STATUSES
        or (label in failed_attempts and label not in final_statuses)
    )
    if not failed_labels:
        return None
    results = []
    remaining = _MAX_GTEST_CASES_EMITTED
    for label in failed_labels:
        result = _gtest_cases_for_target(label, testlogs_root, label in declared_xml,
                                         min(_MAX_FAILURE_LABELS, remaining))
        results.append(result)
        remaining -= len(result["failedCases"])
    return {
        "targets": results,
        "case_names_unavailable": sum(result["status"] == "unavailable" for result in results),
        "omittedCases": sum(result["omittedCases"] for result in results),
    }


def main(argv):
    failures = len(argv) == 3 and argv[1] == "--failures"
    test_cases = len(argv) == 4 and argv[1] == "--test-cases"
    if not failures and not test_cases and len(argv) != 2:
        sys.stderr.write(
            "usage: coverage_bep_status.py [--failures|--test-cases] "
            "<bep.json> [bazel-testlogs]\n")
        return 2
    try:
        bep_path = argv[2] if test_cases else argv[-1]
        with open(bep_path, "r", encoding="utf-8", errors="replace") as stream:
            result = (allowlisted_failure_cases(stream, argv[3]) if test_cases else
                      failure_summary(stream) if failures else classify(stream))
    except OSError:
        result = (None if test_cases else failure_summary([]) if failures else
                  {"status": UNKNOWN, "skipped": [], "produced": []})
    if result is not None:
        json.dump(result, sys.stdout)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
