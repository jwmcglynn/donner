"""Checks browser timing report completeness and disclosure boundaries."""

import json
import unittest

from tools.browser_gpu_test_summary import TARGETS, summarize_events


def events():
    return [{
        "id": {"testResult": {"label": target, "run": 1, "shard": 1, "attempt": 1,
                              "configuration": {"id": "a" * 64}}},
        "testResult": {"status": "PASSED", "testAttemptStartMillisEpoch": "1000",
                       "testAttemptDurationMillis": "20", "cachedLocally": False,
                       "executionInfo": {"strategy": "darwin-sandbox"}},
    } for target in TARGETS]


class BrowserGpuSummaryTest(unittest.TestCase):
    def test_reports_each_fresh_target_and_timing(self):
        result = summarize_events(events())
        self.assertEqual([item["target"] for item in result["tests"]], list(TARGETS))
        self.assertEqual([item["duration_ms"] for item in result["tests"]], [20] * 4)
        self.assertEqual([item["start_ms"] for item in result["tests"]], [1000] * 4)

    def test_discards_unrelated_and_sensitive_fields(self):
        values = events()
        values.append({"optionsParsed": {"cmdLine": "DO_NOT_REPORT"}})
        values[0]["testResult"]["executionInfo"]["hostname"] = "DO_NOT_REPORT"
        values[0]["testResult"]["testActionOutput"] = [{"uri": "DO_NOT_REPORT"}]
        self.assertNotIn("DO_NOT_REPORT", json.dumps(summarize_events(values)))

    def test_missing_duplicate_and_retried_results_are_rejected(self):
        duplicate = events() + [events()[0]]
        retried = events()
        retried[0]["id"]["testResult"]["attempt"] = 2
        for values in (events()[:-1], duplicate, retried):
            with self.subTest(values=len(values)), self.assertRaises(ValueError):
                summarize_events(values)

    def test_cached_results_cannot_qualify_a_comparison(self):
        for field in ("cachedLocally", "cachedRemotely"):
            values = events()
            values[0]["testResult"][field] = True
            with self.subTest(field=field), self.assertRaises(ValueError):
                summarize_events(values)
        values = events()
        values[0]["testResult"]["executionInfo"]["cachedRemotely"] = True
        with self.assertRaises(ValueError):
            summarize_events(values)

    def test_invalid_status_label_configuration_and_timing_are_rejected(self):
        changes = (("status", "DO_NOT_REPORT"), ("testAttemptStartMillisEpoch", True),
                   ("testAttemptStartMillisEpoch", "DO_NOT_REPORT"),
                   ("testAttemptDurationMillis", -1), ("testAttemptDurationMillis", 3600001))
        for field, value in changes:
            values = events()
            values[0]["testResult"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError) as raised:
                summarize_events(values)
            self.assertNotIn("DO_NOT_REPORT", str(raised.exception))
        for field in ("label", "configuration"):
            values = events()
            values[0]["id"]["testResult"][field] = "DO_NOT_REPORT"
            with self.subTest(field=field), self.assertRaises((ValueError, AttributeError)):
                summarize_events(values)

    def test_failures_remain_failures(self):
        values = events()
        values[1]["testResult"]["status"] = "FAILED"
        self.assertEqual(summarize_events(values)["tests"][1]["status"], "FAILED")


if __name__ == "__main__":
    unittest.main()
