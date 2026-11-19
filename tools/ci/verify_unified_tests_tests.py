"""Regression tests for mixed-platform execution evidence."""

import json
from pathlib import Path
import unittest

from python.runfiles import runfiles

from verify_unified_tests import verify


SUITE = "//tools/ci:dual_platform_canary"
METAL = "//tools/ci:unified_metal_preflight"
LABELS = ["//tools/ci:ci_linux_kernel_test", "//tools/ci:ci_macos_kernel_test",
          "//donner/base:ci_linux_ndebug_test", "//donner/base:ci_macos_ndebug_test"]
COLOR = "//donner/gpu/metal/tests:metal_color_matrix_tests_ci_metal"


def events(extra=COLOR, synthetic_extra=True):
    expansions = [{"suiteLabel": SUITE, "testLabels": LABELS},
                  {"suiteLabel": METAL, "testLabels": [extra]}]
    yield {"expanded": {"testSuiteExpansions": expansions}}
    for label in LABELS + ([extra] if synthetic_extra else []):
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
        roots = list(events(extra=label, synthetic_extra=False))
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            verify(roots + records, [SUITE, METAL])  # The recorded results came from the cache.
        for event in records:
            if "testResult" in event:
                event["testResult"].update(cachedLocally=False, executionInfo={"strategy": "remote"})
        verify(roots + records, [SUITE, METAL])
        # Repeating shard 2 cannot satisfy a missing shard 5.
        for event in records:
            identity = event["id"].get("testResult", {})
            if identity.get("shard") == 5:
                identity["shard"] = 2
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            verify(roots + records, [SUITE, METAL])

    def test_ordinary_test_can_reuse_cache(self):
        label = "//pkg:ci_darwin_example"
        shard = {"label": label, "configuration": {"id": "darwin"}, "shard": 1, "run": 1}
        records = list(events()) + [
            {"id": {"targetConfigured": {"label": label}}, "configured": {"testSize": "SMALL"}},
            {"id": {"targetCompleted": {"label": label}}, "completed": {"success": True},
             "children": [{"testResult": shard}]},
            {"id": {"testSummary": {"label": label, "configuration": {"id": "darwin"}}},
             "testSummary": {"overallStatus": "PASSED", "runCount": 1}},
            {"id": {"testResult": shard}, "testResult": {"status": "PASSED", "cachedLocally": True}},
        ]
        verify(records, [SUITE, METAL], [label])
        with self.assertRaisesRegex(ValueError, "incomplete remote shards"):
            verify(records[:-1], [SUITE, METAL], [label])

    def test_missing_build_and_incompatible_headed_browser_fail(self):
        label = "//donner/editor/wasm/tests:ci_darwin_animation_preview_browser_test"
        skipped = {"id": {"targetCompleted": {"label": label}}, "aborted": {"reason": "SKIPPED"}}
        for extra in [[], [skipped]]:
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, "did not finish"):
                verify(list(events()) + extra, [SUITE, METAL], [label])

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
