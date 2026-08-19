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
