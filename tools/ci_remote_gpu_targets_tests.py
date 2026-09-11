"""Tests for exact remote GPU wrapper selection."""

import unittest

from tools.ci_remote_gpu_targets import REMOTE_TARGETS, select_remote_targets


class CiRemoteGpuTargetsTest(unittest.TestCase):
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

    def test_full_fallback_pattern_is_left_for_bazel_tag_selection(self):
        self.assertEqual(["//..."], select_remote_targets(["//..."]))

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
