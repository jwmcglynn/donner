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
import xml.parsers.expat as expat

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

# Pinned to .bazelversion (8.8.0): ExitCode.java and failure_details.proto.
# Never copy the free-form FailureDetail.message, action output, or BEP URIs.
_EXIT_NAMES = {
    0: "SUCCESS", 1: "BUILD_FAILURE", 2: "COMMAND_LINE_ERROR", 3: "TESTS_FAILED",
    4: "NO_TESTS_FOUND", 6: "RUN_FAILURE", 7: "ANALYSIS_FAILURE", 8: "INTERRUPTED",
    9: "LOCK_HELD_NOBLOCK_FOR_LOCK", 32: "REMOTE_ENVIRONMENTAL_ERROR",
    33: "OOM_ERROR", 34: "REMOTE_ERROR", 36: "LOCAL_ENVIRONMENTAL_ERROR",
    37: "BLAZE_INTERNAL_ERROR", 38: "PUBLISH_ERROR", 39: "REMOTE_CACHE_EVICTED",
    45: "PERSISTENT_BUILD_EVENT_SERVICE_UPLOAD_ERROR", 48: "EXTERNAL_DEPS_ERROR",
}
_REMOTE_FAILURE_CODES = {
    "remoteExecution": frozenset({
        "CAPABILITIES_QUERY_FAILURE", "CLIENT_SERVER_INCOMPATIBLE",
        "DOWNLOADED_INPUTS_DELETION_FAILURE", "TOPLEVEL_OUTPUTS_DOWNLOAD_FAILURE",
    }),
    "spawn": frozenset({"EXECUTION_FAILED", "REMOTE_CACHE_FAILED"}),
}
_DIAGNOSTIC_ABORT_REASONS = frozenset({
    "REMOTE_ENVIRONMENT_FAILURE", "TIME_OUT", "INCOMPLETE",
})
_MAX_DIAGNOSTIC_EVENTS = 250000
_MAX_DIAGNOSTIC_LINE_BYTES = 8 * 1024 * 1024


def _empty_failure_context(process_status):
    name = _EXIT_NAMES.get(process_status) if type(process_status) is int else None
    return {
        "schemaVersion": 1,
        "processExitCode": process_status if name else "unavailable",
        "processExitName": name or "unavailable",
        "bepStatus": "missing",
        "bepExitCode": "unavailable",
        "failureCategory": "unavailable",
        "failureCode": "unavailable",
        "abortReasons": [],
    }


def _pinned_remote_detail(finished):
    """Return only an exit-34 FailureDetail category/code pair from Bazel 8.8."""
    detail = finished.get("failureDetail")
    if not isinstance(detail, dict):
        return None
    categories = set(detail) - {"message"}
    if len(categories) != 1:
        return None
    category = categories.pop()
    allowed = _REMOTE_FAILURE_CODES.get(category)
    payload = detail.get(category)
    code = payload.get("code") if isinstance(payload, dict) else None
    if allowed is None or not isinstance(code, str) or code not in allowed:
        return None
    return category, code


def _context_event(line):
    if len(line) > _MAX_DIAGNOSTIC_LINE_BYTES:
        return None
    try:
        event = json.loads(line)
    except (ValueError, RecursionError):
        return None
    return event if isinstance(event, dict) else None


def _known_abort_reason(event):
    aborted = event.get("aborted")
    if not isinstance(aborted, dict):
        return None
    reason = aborted.get("reason")
    return reason if isinstance(reason, str) and reason in _DIAGNOSTIC_ABORT_REASONS else None


def _scan_context_events(lines):
    """Collect only a final event and pinned abort reasons; fail closed on damage."""
    finished = None
    reasons = set()
    for index, line in enumerate(lines):
        if index >= _MAX_DIAGNOSTIC_EVENTS:
            return None, [], True
        event = _context_event(line)
        if event is None:
            return None, [], True
        reason = _known_abort_reason(event)
        if reason is not None:
            reasons.add(reason)
        event_id = event.get("id")
        if not isinstance(event_id, dict) or "finished" not in event_id:
            continue
        if finished is not None:
            return None, [], True
        finished = event.get("finished")
        if not isinstance(finished, dict):
            return None, [], True
    return finished, sorted(reasons), False


def _validated_finished_name(finished, process_status):
    exit_code = finished.get("exitCode")
    if not isinstance(exit_code, dict):
        return None
    code = exit_code.get("code")
    name = exit_code.get("name")
    if type(code) is int and code == process_status and _EXIT_NAMES.get(code) == name:
        return name
    return None


def failure_context(lines, process_status):
    """Summarize a failed Bazel invocation using only pinned structured enums.

    Malformed, partial, duplicate, and mismatched streams never publish raw
    BEP text or an inferred root cause. The process status is recorded only
    when it belongs to Bazel 8.8's fixed ExitCode table.
    """
    result = _empty_failure_context(process_status)
    finished, reasons, malformed = _scan_context_events(lines)
    if malformed:
        result["bepStatus"] = "malformed"
        return result
    result["abortReasons"] = reasons
    if finished is None:
        return result
    name = _validated_finished_name(finished, process_status)
    if name is None:
        result["bepStatus"] = "mismatch"
        return result
    result["bepStatus"] = "finished"
    result["bepExitCode"] = name
    if process_status == 34:
        detail = _pinned_remote_detail(finished)
        if detail is not None:
            result["failureCategory"], result["failureCode"] = detail
    return result



_FAILURE_CONTEXT_KEYS = frozenset({
    "schemaVersion", "processExitCode", "processExitName", "bepStatus",
    "bepExitCode", "failureCategory", "failureCode", "abortReasons",
})
_TIMING_PHASES = ("start", "bazel_coverage_done", "filter_done", "end")


def _valid_exit_pair(code, name):
    if code == "unavailable" and name == "unavailable":
        return True
    return (type(code) is int and code in _EXIT_NAMES
            and isinstance(name, str) and _EXIT_NAMES[code] == name)


def _valid_context_codes(value):
    category = value["failureCategory"]
    code = value["failureCode"]
    if not isinstance(category, str) or not isinstance(code, str):
        return False
    if category == "unavailable":
        return code == "unavailable"
    return (value["processExitCode"] == 34
            and code in _REMOTE_FAILURE_CODES.get(category, ()))


def _valid_abort_reasons(reasons):
    return (isinstance(reasons, list) and len(reasons) <= len(_DIAGNOSTIC_ABORT_REASONS)
            and all(isinstance(reason, str) and reason in _DIAGNOSTIC_ABORT_REASONS
                    for reason in reasons) and reasons == sorted(set(reasons)))


def _valid_failure_context(value):
    if not isinstance(value, dict) or set(value) != _FAILURE_CONTEXT_KEYS:
        return False
    if type(value["schemaVersion"]) is not int or value["schemaVersion"] != 1:
        return False
    if not _valid_exit_pair(value["processExitCode"], value["processExitName"]):
        return False
    if not isinstance(value["bepStatus"], str) or value["bepStatus"] not in {
            "missing", "malformed", "mismatch", "finished"}:
        return False
    if not isinstance(value["bepExitCode"], str) or value["bepExitCode"] not in (
            set(_EXIT_NAMES.values()) | {"unavailable"}):
        return False
    return (_valid_context_codes(value)
            and _valid_abort_reasons(value["abortReasons"]))


def _bounded_regular_text(path, limit):
    try:
        if path.is_symlink() or not path.is_file():
            return None
        with path.open("r", encoding="utf-8") as stream:
            value = stream.read(limit + 1)
        return value if len(value) <= limit else None
    except (OSError, UnicodeError):
        return None


def _valid_timing(value):
    if value is None:
        return False
    lines = value.splitlines(keepends=True)
    if not lines or len(lines) > len(_TIMING_PHASES):
        return False
    for phase, line in zip(_TIMING_PHASES, lines):
        prefix = phase + "="
        if not line.startswith(prefix) or not line.endswith("\n"):
            return False
        digits = line[len(prefix):-1]
        if not digits.isascii() or not digits.isdigit() or not 1 <= len(digits) <= 12:
            return False
    return True


def validate_diagnostics(directory):
    """Allow the workflow to upload only fresh, fixed-schema regular files."""
    timing = _bounded_regular_text(directory / "timing.txt", 128)
    if not _valid_timing(timing):
        return False
    summary_path = directory / "failure-summary.json"
    if not summary_path.exists() and not summary_path.is_symlink():
        return True
    summary = _bounded_regular_text(summary_path, 1024)
    if summary is None:
        return False
    try:
        return _valid_failure_context(json.loads(summary))
    except (ValueError, RecursionError):
        return False


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


def _read_test_xml(label, testlogs_root):
    """Return bounded bytes from a fixed Bazel path or a fixed error code."""
    try:
        root_path = Path(testlogs_root).resolve(strict=True)
        if not root_path.is_dir():
            return None, "missing_xml"
        xml_path = root_path
        for component in _GTEST_XML_BY_LABEL[label].parts:
            xml_path /= component
            if xml_path.is_symlink():
                return None, "symlink_xml"
        if not xml_path.resolve(strict=True).is_relative_to(root_path):
            return None, "path_escape"
    except (OSError, RuntimeError):
        return None, "missing_xml"
    try:
        with open(xml_path, "rb") as stream:
            document = stream.read(_MAX_GTEST_XML_BYTES + 1)
    except OSError:
        return None, "missing_xml"
    if len(document) > _MAX_GTEST_XML_BYTES:
        return None, "oversize_xml"
    if b"<!DOCTYPE" in document.upper() or b"<!ENTITY" in document.upper():
        return None, "invalid_xml"
    return document, None


class _GTestXmlCases:
    """Collect tag names only; never retain XML text or assertion attributes."""

    def __init__(self):
        self.depth = 0
        self.root_name = None
        self.root_failures = None
        self.root_errors = "0"
        self.current_case = None
        self.current_case_depth = 0
        self.current_failed = False
        self.failed_cases = []
        self.case_count = 0

    def start(self, name, attributes):
        self.depth += 1
        if self.depth == 1:
            self.root_name = name
            self.root_failures = attributes.get("failures")
            self.root_errors = attributes.get("errors", "0")
        if name == "testcase":
            self.case_count += 1
            if self.case_count > _MAX_GTEST_CASES_SCANNED or self.current_case is not None:
                raise ValueError("too many or nested cases")
            self.current_case = (attributes.get("classname"), attributes.get("name"))
            self.current_case_depth = self.depth
            self.current_failed = False
        elif (name in ("failure", "error") and self.current_case is not None
              and self.depth == self.current_case_depth + 1):
            self.current_failed = True

    def end(self, name):
        if name == "testcase" and self.current_case is not None:
            if self.current_failed:
                self.failed_cases.append(self.current_case)
            self.current_case = None
        self.depth -= 1


def _reject_xml_declaration(*_args):
    raise ValueError("XML declarations are not accepted")


def _parse_gtest_cases(document):
    """Parse bounded XML with entity declarations disabled and validate counts."""
    cases = _GTestXmlCases()
    parser = expat.ParserCreate()
    parser.StartElementHandler = cases.start
    parser.EndElementHandler = cases.end
    parser.StartDoctypeDeclHandler = _reject_xml_declaration
    parser.EntityDeclHandler = _reject_xml_declaration
    parser.ExternalEntityRefHandler = lambda *_args: 0
    try:
        parser.Parse(document, True)
        reported_failures = int(cases.root_failures) + int(cases.root_errors)
    except (expat.ExpatError, TypeError, ValueError):
        return None, "invalid_xml"
    if (cases.root_name not in ("testsuites", "testsuite") or reported_failures < 1
            or reported_failures > _MAX_GTEST_CASES_SCANNED):
        return None, "invalid_xml"
    failed_cases = []
    for suite, name in cases.failed_cases:
        if (not isinstance(suite, str) or not isinstance(name, str)
                or len(suite) > 80 or len(name) > 80
                or not _SAFE_GTEST_NAME.fullmatch(suite)
                or not _SAFE_GTEST_NAME.fullmatch(name)):
            return None, "unsafe_case_name"
        failed_cases.append(f"{suite}.{name}")
    if not failed_cases or len(set(failed_cases)) != reported_failures:
        return None, "incomplete_xml"
    return sorted(set(failed_cases)), None


def _gtest_cases_for_target(label, testlogs_root, declared_xml, output_cap):
    """Report identifiers only for a BEP-declared allowlisted failed target."""
    reason = "undeclared_xml" if not declared_xml else None
    if reason is None:
        document, reason = _read_test_xml(label, testlogs_root)
    if reason is None:
        failed_cases, reason = _parse_gtest_cases(document)
    if reason is not None:
        return {
            "target": label,
            "status": "unavailable",
            "reason": reason,
            "failedCases": [],
            "omittedCases": 0,
        }
    return {
        "target": label,
        "status": "cases_found",
        "failedCases": failed_cases[:output_cap],
        "failedCaseCount": len(failed_cases),
        "omittedCases": max(0, len(failed_cases) - output_cap),
    }


def _allowlisted_events(lines):
    """Yield only BEP events for the three fixed GPU test targets."""
    for line in lines:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict):
            label = _label(event.get("id"))
            if label in _GTEST_XML_BY_LABEL:
                yield label, event


def _declares_test_xml(event):
    attempt = event.get("testResult")
    outputs = attempt.get("testActionOutput") if isinstance(attempt, dict) else None
    return isinstance(outputs, list) and any(
        isinstance(output, dict) and output.get("name") == "test.xml"
        for output in outputs)


def _allowlisted_test_statuses(lines):
    """Final BEP summaries override failed attempts, as in failure_summary."""
    final_statuses = {}
    failed_attempts = set()
    declared_xml = set()
    for label, event in _allowlisted_events(lines):
        status = _final_test_status(event)
        if status is not None:
            final_statuses[label] = status
        if _failed_test_attempt(event):
            failed_attempts.add(label)
        if _declares_test_xml(event):
            declared_xml.add(label)
    return final_statuses, failed_attempts, declared_xml


def allowlisted_failure_cases(lines, testlogs_root):
    """Emit bounded case names for three named GPU tests, never raw XML text."""
    final_statuses, failed_attempts, declared_xml = _allowlisted_test_statuses(lines)
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


def _cli_mode(argv):
    if len(argv) == 2:
        return "classify"
    if len(argv) == 3 and argv[1] == "--failures":
        return "failures"
    if len(argv) == 3 and argv[1] == "--validate-diagnostics":
        return "validate_diagnostics"
    if len(argv) == 4 and argv[1] == "--test-cases":
        return "test_cases"
    if len(argv) == 4 and argv[1] == "--failure-context":
        return "context"
    return None


def _cli_process_status(argv, mode):
    if mode != "context":
        return None
    try:
        return int(argv[3])
    except ValueError:
        return None


def _cli_result(mode, stream, extra):
    if mode == "test_cases":
        return allowlisted_failure_cases(stream, extra)
    if mode == "context":
        return failure_context(stream, extra)
    if mode == "failures":
        return failure_summary(stream)
    return classify(stream)


def _cli_missing_result(mode, extra):
    if mode == "test_cases":
        return None
    if mode == "context":
        return failure_context([], extra)
    if mode == "failures":
        return failure_summary([])
    return {"status": UNKNOWN, "skipped": [], "produced": []}


def main(argv):
    mode = _cli_mode(argv)
    if mode is None:
        sys.stderr.write(
            "usage: coverage_bep_status.py [--failures|--test-cases|--failure-context] "
            "<bep.json> [bazel-testlogs|process-status]\n")
        return 2
    if mode == "validate_diagnostics":
        return 0 if validate_diagnostics(Path(argv[2])) else 1
    extra = argv[3] if mode == "test_cases" else _cli_process_status(argv, mode)
    bep_path = argv[2] if mode in {"test_cases", "context"} else argv[-1]
    try:
        with open(bep_path, "r", encoding="utf-8", errors="replace") as stream:
            result = _cli_result(mode, stream, extra)
    except OSError:
        result = _cli_missing_result(mode, extra)
    if result is not None:
        json.dump(result, sys.stdout)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
