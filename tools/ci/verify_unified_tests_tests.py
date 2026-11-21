"""Regression tests for mixed-platform execution evidence."""

import json
from pathlib import Path
import tempfile
import unittest

from python.runfiles import runfiles

from verify_unified_tests import ARCHIVE, Evidence, KERNELS, darwin_classes, editor_browser_cases, firefox_cases, report_kernels, verify


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

    def test_selected_linux_hardware_variant_requires_fresh_remote_results(self):
        label = "//pkg:ci_linux_gpu_device_test"
        target = {"label": label, "configuration": {"id": "gpu"}}
        shard = {**target, "shard": 1, "run": 1}
        records = list(events()) + [
            {"id": {"targetCompleted": target}, "completed": {"success": True}},
            {"id": {"testSummary": target}, "testSummary": {"overallStatus": "PASSED", "runCount": 1}},
        ]
        for strategy, cached in [("remote", False), ("remote", True), ("local", False)]:
            result = {"id": {"testResult": shard}, "testResult": {"status": "PASSED",
                      "cachedLocally": cached, "executionInfo": {"strategy": strategy}}}
            with self.subTest(strategy=strategy, cached=cached):
                if strategy == "remote" and not cached:
                    verify(records + [result], [SUITE, METAL], linux=[label])
                else:
                    with self.assertRaises(ValueError):
                        verify(records + [result], [SUITE, METAL], linux=[label])
        skipped = {"id": {"targetCompleted": target}, "aborted": {"reason": "SKIPPED"}}
        with self.assertRaises(ValueError):
            verify(list(events()) + [skipped], [SUITE, METAL], linux=[label])

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

    def test_aspect_cannot_establish_a_target_outcome(self):
        label = "//pkg:platform_test"
        target = {"label": label, "configuration": {"id": "linux"}}
        configured = {"id": {"targetConfigured": {"label": label}}, "configured": {"testSize": "SMALL"}}
        aspect = {"id": {"targetCompleted": dict(target, aspect="//pkg:venue.bzl%venue")},
                  "completed": {"success": True}}
        skipped = {"id": {"targetCompleted": target}, "aborted": {"reason": "SKIPPED"}}
        built = {"id": {"targetCompleted": target}, "completed": {"success": True}}
        records = list(events()) + [configured]
        for pair in ([skipped, aspect], [aspect, skipped]):
            verify(records + pair, [SUITE, METAL], linux=[label])
        with self.assertRaisesRegex(ValueError, "did not finish"):
            verify(records + [aspect], [SUITE, METAL], linux=[label])
        with self.assertRaisesRegex(ValueError, "Test label mismatch"):
            verify(records + [built, aspect], [SUITE, METAL], linux=[label])

    def test_missing_build_and_incompatible_headed_browser_fail(self):
        label = "//donner/editor/wasm/tests:ci_darwin_animation_preview_browser_test"
        skipped = {"id": {"targetCompleted": {"label": label}}, "aborted": {"reason": "SKIPPED"}}
        for extra in [[], [skipped]]:
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, "did not finish"):
                verify(list(events()) + extra, [SUITE, METAL], [label])

    def test_selected_wasm_payload_check_cannot_be_platform_skipped(self):
        linux = "//donner/svg/renderer/wasm:graph_wasm_size_tests"
        darwin = "//donner/editor/wasm:ci_darwin_graph_wasm_geode_package_size_tests"
        for label, kwargs in [(linux, {"linux": [linux]}), (darwin, {"ordinary": [darwin]})]:
            skip = {"id": {"targetCompleted": {"label": label}}, "aborted": {"reason": "SKIPPED"}}
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "did not finish"):
                verify(list(events()) + [skip], [SUITE, METAL], **kwargs)

    def test_full_graph_cannot_omit_the_editor_qualification(self):
        with self.assertRaisesRegex(ValueError, "Full graph omitted editor Wasm targets"):
            verify(events(), [SUITE, METAL], full_graph=True)

    def test_kernel_log_must_match_its_platform_and_declared_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            locations = {}
            for label, kernel in KERNELS.items():
                path = root / "config/testlogs/tools/ci" / label.split(":")[1] / "test.log"
                path.parent.mkdir(parents=True)
                path.write_text(f"test worker: {kernel}\n")
                locations[label] = path.as_uri()
            report_kernels(locations, root)
            path.write_text("test worker: Linux x86_64\n")
            with self.assertRaisesRegex(ValueError, "architecture evidence"):
                report_kernels(locations, root)
            with self.assertRaisesRegex(ValueError, "log location"):
                report_kernels(locations, path.parent)

    def test_receipt_counts_only_verified_work_of_each_darwin_class(self):
        evidence = Evidence()
        self.assertEqual(darwin_classes(evidence, set()), {})
        evidence.members = {"//tools/ci:unified_editor_geode": {"g1", "g2"},
                            "//tools/ci:unified_mac_browser_archive": {ARCHIVE}}
        evidence.test_rules = {"headless", "headed", ARCHIVE}
        evidence.tags = {"headless": {"launches-browser"}, "headed": {"launches-browser", "requires_desktop_session"},
                         ARCHIVE: {"launches-browser"}}
        counts = darwin_classes(evidence, {"binary", "headless", "headed", ARCHIVE})
        self.assertEqual(counts, {"darwin-build": 4, "darwin-test": 3, "geode": 2,
                                  "browser-archive": 1, "headless-browser": 1, "desktop-browser": 1})
        self.assertNotIn("metal-profile", counts)

    def test_editor_browser_receipt_requires_one_positive_execution_count(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "test.log"
            log.write_text("Running 12 tests\nEditor browser cases passed: 10\n")
            self.assertEqual(editor_browser_cases(log), 10)
            for body in ("12 skipped", "Editor browser cases passed: 0",
                         "Editor browser cases passed: 10\nEditor browser cases passed: 10"):
                with self.subTest(body=body), self.assertRaisesRegex(ValueError, "case count"):
                    log.write_text(body)
                    editor_browser_cases(log)

    def test_headed_firefox_requires_executed_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "test.log"
            log.write_bytes(b"Browser output\n  \x1b[32m4 passed\x1b[39m (1.3m)\n")
            self.assertEqual(firefox_cases(log), 4)
            for trailer in ("1 skipped", "0 passed", "3 passed (50s)\n1 skipped",
                            "1 flaky\n4 passed (2m)", "no test output"):
                with self.subTest(trailer=trailer), self.assertRaisesRegex(ValueError, "complete passing"):
                    log.write_text(trailer)
                    firefox_cases(log)

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
