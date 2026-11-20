"""Tests for exact remote GPU wrapper selection."""

import json
import os
import subprocess
import unittest

from python.runfiles import runfiles

from tools import test_timing
from tools.ci_remote_gpu_targets import (
    AUDIT, DISPLAY_TESTS, REFERENCE, REMOTE_TARGETS, SOAK, check_results, linux_native_patterns, select_remote_targets,
)


class CiRemoteGpuTargetsTest(unittest.TestCase):
    def run_cli(self, *args):
        resolver = runfiles.Create()
        return subprocess.run(
            [resolver.Rlocation("donner/tools/ci_remote_gpu_targets"), *args],
            env={**os.environ, **resolver.EnvVars()},
            check=False,
            capture_output=True,
            text=True,
            timeout=test_timing.STALL_GUARD_SECONDS,
        )

    def test_mixed_graph_preserves_build_only_and_remote_test_roots(self):

        def rule(label, *tags):
            return json.dumps({"rule": {"name": label, "attribute": [
                {"name": "tags", "stringListValue": tags}]}})

        local = "//donner/editor/tests:rnr_replay_tests_geode"
        roots = linux_native_patterns([rule(local, "local-gpu-isolated"), rule("//pkg:perf", "perf"),
                                      rule("//pkg:normal"), rule("//pkg:soak", "fuzz_soak")])
        self.assertEqual(set(roots), {REMOTE_TARGETS[local], "//donner/editor/tests:ci_linux_build_rnr_replay_tests_geode",
                                     "//pkg:ci_linux_build_perf", "//pkg:normal", "//pkg:ci_linux_build_soak"})

    def test_mapping_is_limited_to_measured_sharded_suites(self):
        self.assertEqual(
            {
                "//donner/editor/tests:rnr_replay_tests_geode":
                    "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
                "//donner/svg/renderer/tests:resvg_test_suite_geode":
                    "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote",
                "//donner/svg/renderer/tests:resvg_test_suite_geode_max_text":
                    "//donner/svg/renderer/tests:resvg_test_suite_geode_max_text_ci_remote",
            },
            REMOTE_TARGETS,
        )

    def test_mixed_execution_receipt(self):
        display, other_display = sorted(DISPLAY_TESTS)
        ordinary = "//other:ordinary_test"

        def result(label, strategy, cached=False):
            return json.dumps({"id": {"testResult": {"label": label}}, "testResult": {
                "status": "PASSED", "cachedLocally": cached, "executionInfo": {"strategy": strategy}}})

        finished = json.dumps({"finished": {}})
        check_results([result(display, "local"), result(other_display, "local"),
                       result(ordinary, "remote"), result(ordinary, None, True), finished], ["//..."])
        check_results([finished], [ordinary])
        for events, selected in (
                ([result(display, "local"), finished], ["//..."]),
                ([finished], [display]),
                ([result(display, "remote"), finished], [display]),
                ([result(display, "local", True), finished], [display]),
                ([result(ordinary, "local"), finished], [ordinary]),
                ([], [ordinary])):
            with self.subTest(events=events), self.assertRaises(ValueError):
                check_results(events, selected)

    def test_merged_graph_requires_the_probe_and_every_selected_reference_shard(self):
        def event(label, shard=1, cached=False, status="PASSED"):
            return json.dumps({"id": {"testResult": {"label": label, "shard": shard}},
                               "testResult": {"status": status, "cachedLocally": cached,
                                              "executionInfo": {"strategy": "remote"}}})

        end = json.dumps({"finished": {}})
        probe = event(SOAK)
        reference = [event(REFERENCE, i, cached=True) for i in range(1, 17)]
        audit = event(AUDIT)
        check_results([probe, end], [], fixed=True)
        check_results([probe, *reference, audit, end], [], fixed=True, reference=True)
        check_results([*reference, audit, end], [], fixed=True, reference=True, reference_only=True)
        for requested, rows in ((False, []), (False, [event(SOAK, cached=True)]),
                                (False, [event(SOAK, status="FAILED")]), (False, [probe, audit]),
                                (True, [probe, *reference]), (True, [*reference, audit]),
                                (True, [probe, *reference[:-1], reference[0], audit])):
            with self.subTest(requested=requested, count=len(rows)), self.assertRaises(ValueError):
                check_results([*rows, end], [], fixed=True, reference=requested)

    def test_empty_selection_stays_empty(self):
        self.assertEqual([], select_remote_targets([]))

    def test_empty_cli_selection_emits_no_array_element(self):
        result = self.run_cli("--one-per-line")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("", result.stdout)

    def test_full_fallback_pattern_is_left_for_bazel_tag_selection(self):
        self.assertEqual(["//..."], select_remote_targets(["//..."]))

    def test_invalid_cli_option_fails(self):
        result = self.run_cli("--invalid")
        self.assertEqual(2, result.returncode, result.stderr)
        self.assertIn("unrecognized arguments: --invalid", result.stderr)

    def test_nonempty_cli_selection_is_exact(self):
        result = self.run_cli(
            "//donner/editor/tests:rnr_replay_tests_geode", "//other:test"
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "//donner/editor/tests:rnr_replay_tests_geode_ci_remote //other:test\n",
            result.stdout,
        )

    def test_only_exact_opted_in_variants_are_replaced(self):
        self.assertEqual(
            [
                "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
                "//donner/editor/tests:rnr_replay_tests",
                "//other:rnr_replay_tests_geode",
            ],
            select_remote_targets(
                [
                    "//donner/editor/tests:rnr_replay_tests_geode",
                    "//donner/editor/tests:rnr_replay_tests",
                    "//other:rnr_replay_tests_geode",
                ]
            ),
        )

    def test_already_expanded_targets_are_deduplicated(self):
        remote = "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote"
        self.assertEqual(
            [remote, "//tools:ci_runtime_workflow_tests"],
            select_remote_targets(
                [
                    "//donner/svg/renderer/tests:resvg_test_suite_geode",
                    remote,
                    "//tools:ci_runtime_workflow_tests",
                ]
            ),
        )


if __name__ == "__main__":
    unittest.main()
