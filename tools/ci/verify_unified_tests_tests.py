"""Regression tests for mixed-platform execution evidence."""

import unittest

from verify_unified_tests import verify


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
        yield {"completed": {"success": True}, "children": [{"testResult": s} for s in shards]}
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
