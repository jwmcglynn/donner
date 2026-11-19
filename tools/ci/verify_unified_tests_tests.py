"""Regression tests for mixed-platform execution evidence."""

import json
from pathlib import Path
import unittest

from python.runfiles import runfiles

from verify_unified_tests import _collect, verify


SUITE = "//tools/ci:dual_platform_canary"
METAL = "//tools/ci:unified_metal_preflight"
LABELS = ["//tools/ci:ci_linux_kernel_test", "//tools/ci:ci_macos_kernel_test",
          "//donner/base:ci_linux_ndebug_test", "//donner/base:ci_macos_ndebug_test"]
COLOR = "//donner/gpu/metal/tests:metal_color_matrix_tests_ci_metal"


def events():
    expansions = [{"suiteLabel": SUITE, "testLabels": LABELS},
                  {"suiteLabel": METAL, "testLabels": [COLOR]}]
    yield {"expanded": {"testSuiteExpansions": expansions}}
    for label in [*LABELS, COLOR]:
        shards = [{"label": label, "configuration": {"id": "abc"}, "shard": shard, "run": 1}
                  for shard in range(1, 3 if label == COLOR else 2)]
        identity = {"label": label, "configuration": {"id": "abc"}}
        yield {"completed": {"success": True}, "children": [{"testSummary": identity}]}
        yield {"id": {"testSummary": identity}, "testSummary": {
            "overallStatus": "PASSED", "runCount": 1, "shardCount": len(shards)}}
        for shard in shards:
            yield {"id": {"testResult": shard}, "testResult": {
                "status": "PASSED", "executionInfo": {"strategy": "remote"}}}


class UnifiedTest(unittest.TestCase):
    def test_all_requested_shards_executed(self):
        verify(events(), [SUITE, METAL])

    def test_missing_shard_or_suite(self):
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            verify(list(events())[:-1], [SUITE, METAL])
        with self.assertRaisesRegex(ValueError, "No test suite expansion"):
            verify(events(), [SUITE, METAL + "_missing"])

    def test_recorded_summary_only_completion_with_five_shards(self):
        fixture = runfiles.Create().Rlocation("donner/tools/testdata/macos_remote_shadow_build_events.json")
        label = "//donner/base/parser:parser_tests"
        records = []
        for line in Path(fixture).read_text().splitlines():
            event = json.loads(line)
            if next(iter(event["id"].values())).get("label") == label:
                records.append(event)
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            _collect(records)  # The recorded results came from the cache.
        for event in records:
            if "testResult" in event:
                event["testResult"].update(cachedLocally=False, executionInfo={"strategy": "remote"})
        self.assertEqual(len(_collect(records)[1]), 5)
        # Repeating shard 2 cannot satisfy a missing shard 5.
        for event in records:
            identity = event["id"].get("testResult", {})
            if identity.get("shard") == 5:
                identity["shard"] = 2
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            _collect(records)

    def test_shortened_canary_cannot_pass(self):
        records = list(events())
        records[0]["expanded"]["testSuiteExpansions"][0]["testLabels"] = LABELS[:3]
        with self.assertRaisesRegex(ValueError, "canary coverage"):
            verify(records, [SUITE, METAL])

    def test_rejects_cached_local_failed_or_skipped_results(self):
        for change in [{"cachedLocally": True}, {"executionInfo": {"strategy": "local"}},
                       {"executionInfo": {"strategy": "remote", "cachedRemotely": True}},
                       {"status": "FAILED"}, {"status": "INCOMPLETE"}]:
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "incomplete remote shards"):
                records = list(events())
                records[-1]["testResult"].update(change)
                verify(records, [SUITE, METAL])


if __name__ == "__main__":
    unittest.main()
