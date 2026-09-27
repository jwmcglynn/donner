"""Tests for the coverage BEP classifier.

The classifier decides whether a missing coverage report is benign, so the
interesting cases are the ones where it must REFUSE to say "benign": a real
build failure, a partial stream, and a run that mixed a skip with a result.
"""

#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import coverage_bep_status as status


def skipped_event(label):
    return json.dumps(
        {
            "id": {"targetCompleted": {"label": label}},
            "aborted": {
                "reason": "SKIPPED",
                "description": "Target %s build was skipped." % label,
            },
        }
    )


def completed_event(label, success=True):
    return json.dumps(
        {"id": {"targetCompleted": {"label": label}}, "completed": {"success": success}}
    )


def test_result_event(label):
    return json.dumps(
        {"id": {"testResult": {"label": label}}, "testResult": {"status": "PASSED"}}
    )


class ClassifyTest(unittest.TestCase):
    def test_failure_summary_reports_only_safe_bounded_labels(self):
        events = [
            json.dumps({
                "id": {"testSummary": {"label": "//donner/editor:failed_test"}},
                "testSummary": {"overallStatus": "FAILED", "passed": [{"uri": "file:///private/path"}]},
            }),
            json.dumps({
                "id": {"testResult": {"label": "//donner/svg:timed_out"}},
                "testResult": {"status": "TIMEOUT"},
            }),
            completed_event("//tools:failed_build", success=False),
            json.dumps({
                "id": {"targetSummary": {"label": "@@lib+//pkg:compile_test"}},
                "targetSummary": {"overallTestStatus": "FAILED_TO_BUILD"},
            }),
            json.dumps({
                "id": {"configuredLabel": {"label": "//donner/editor:analysis_failed"}},
                "aborted": {"reason": "ANALYSIS_FAILURE", "description": "/private/path"},
            }),
            json.dumps({
                "id": {"testResult": {"label": "//donner/editor:halted"}},
                "testResult": {"status": "TOOL_HALTED_BEFORE_TESTING"},
            }),
            json.dumps({
                "id": {"testSummary": {"label": "//donner/editor:malformed"}},
                "testSummary": {"overallStatus": []},
            }),
            json.dumps({
                "id": {"testSummary": {"label": "//evil\nprivate:path"}},
                "testSummary": {"overallStatus": "FAILED"},
            }),
        ]
        result = status.failure_summary(events)
        self.assertEqual(result["failedTests"], [
            "//donner/editor:failed_test", "//donner/editor:halted",
            "//donner/svg:timed_out", "@@lib+//pkg:compile_test",
        ])
        self.assertEqual(result["failedBuilds"], [
            "//donner/editor:analysis_failed", "//tools:failed_build",
        ])
        self.assertNotIn("private", json.dumps(result))
        self.assertEqual(result["omittedTests"], 0)

    def test_failure_summary_caps_output(self):
        events = [json.dumps({
            "id": {"testSummary": {"label": f"//fixture:failed_{index:02}"}},
            "testSummary": {"overallStatus": "FAILED"},
        }) for index in range(23)]
        result = status.failure_summary(events)
        self.assertEqual(len(result["failedTests"]), 20)
        self.assertEqual(result["omittedTests"], 3)

    def test_final_pass_or_flaky_summary_supersedes_failed_attempt(self):
        events = []
        for label, final_status in (("//fixture:recovered", "PASSED"),
                                    ("//fixture:flaky", "FLAKY")):
            events.append(json.dumps({
                "id": {"testResult": {"label": label}},
                "testResult": {"status": "FAILED"},
            }))
            events.append(json.dumps({
                "id": {"testSummary": {"label": label}},
                "testSummary": {"overallStatus": final_status},
            }))
        events.append(json.dumps({
            "id": {"testResult": {"label": "//fixture:incomplete"}},
            "testResult": {"status": "TIMEOUT"},
        }))
        self.assertEqual(status.failure_summary(events)["failedTests"],
                         ["//fixture:incomplete"])

    def test_single_incompatible_target_is_all_skipped(self):
        # The real shape this was written for: a pull request whose only
        # affected target is restricted to another platform.
        result = status.classify([skipped_event("//donner/editor/wasm/tests:smoke")])
        self.assertEqual(result["status"], status.ALL_SKIPPED)
        self.assertEqual(result["skipped"], ["//donner/editor/wasm/tests:smoke"])

    def test_every_target_skipped_is_all_skipped(self):
        result = status.classify(
            [skipped_event("//a:one"), skipped_event("//b:two")]
        )
        self.assertEqual(result["status"], status.ALL_SKIPPED)
        self.assertEqual(result["skipped"], ["//a:one", "//b:two"])

    def test_one_result_alongside_a_skip_is_not_benign(self):
        # A target really ran, so a missing report means something went wrong
        # with the report, not that there was nothing to measure.
        result = status.classify([skipped_event("//a:one"), completed_event("//b:two")])
        self.assertEqual(result["status"], status.HAS_RESULTS)
        self.assertEqual(result["produced"], ["//b:two"])

    def test_test_result_counts_as_a_result(self):
        result = status.classify([test_result_event("//a:one")])
        self.assertEqual(result["status"], status.HAS_RESULTS)

    def test_failed_completion_is_not_a_result_but_is_not_benign_either(self):
        # An unsuccessful completion with no skip is the ordinary build-failure
        # case; it must stay unknown so the caller keeps failing closed.
        result = status.classify([completed_event("//a:one", success=False)])
        self.assertEqual(result["status"], status.UNKNOWN)

    def test_empty_stream_is_unknown(self):
        self.assertEqual(status.classify([])["status"], status.UNKNOWN)
        self.assertEqual(status.classify(["", "  "])["status"], status.UNKNOWN)

    def test_unparseable_stream_is_unknown(self):
        self.assertEqual(status.classify(["not json"])["status"], status.UNKNOWN)

    def test_partial_trailing_line_does_not_hide_a_skip(self):
        # A run killed mid-write leaves a truncated final line; the completed
        # events before it still classify.
        result = status.classify([skipped_event("//a:one"), '{"id": {"targetCom'])
        self.assertEqual(result["status"], status.ALL_SKIPPED)

    def test_non_object_json_lines_are_ignored(self):
        self.assertEqual(status.classify(["[1,2,3]", "null"])["status"], status.UNKNOWN)

    def test_missing_file_is_unknown(self):
        self.assertEqual(status.main(["prog", "/nonexistent/bep.json"]), 0)


if __name__ == "__main__":
    unittest.main()
