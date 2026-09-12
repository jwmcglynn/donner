"""Tests for exact remote GPU wrapper selection."""

import os
import subprocess
import unittest

from python.runfiles import runfiles

from tools.ci_remote_gpu_targets import REMOTE_TARGETS, select_remote_targets


class CiRemoteGpuTargetsTest(unittest.TestCase):
    def run_cli(self, *args):
        resolver = runfiles.Create()
        return subprocess.run(
            [resolver.Rlocation("donner/tools/ci_remote_gpu_targets"), *args],
            env={**os.environ, **resolver.EnvVars()},
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_mapping_is_limited_to_measured_sharded_suites(self):
        self.assertEqual(
            {
                "//donner/editor/tests:rnr_replay_tests_geode":
                    "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
                "//donner/svg/renderer/tests:resvg_test_suite_geode":
                    "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote",
            },
            REMOTE_TARGETS,
        )

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
