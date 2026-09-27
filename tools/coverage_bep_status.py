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
import re
import sys

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


def main(argv):
    failures = len(argv) == 3 and argv[1] == "--failures"
    if not failures and len(argv) != 2:
        sys.stderr.write("usage: coverage_bep_status.py [--failures] <bep.json>\n")
        return 2
    try:
        with open(argv[-1], "r", encoding="utf-8", errors="replace") as stream:
            result = failure_summary(stream) if failures else classify(stream)
    except OSError:
        result = (failure_summary([]) if failures else
                  {"status": UNKNOWN, "skipped": [], "produced": []})
    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
