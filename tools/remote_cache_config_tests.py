import re
import unittest

from python.runfiles import runfiles


class RemoteCacheConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        bazelrc_path = resolver.Rlocation("donner/.bazelrc")
        bazelversion_path = resolver.Rlocation("donner/.bazelversion")
        coverage_workflow_path = resolver.Rlocation(
            "donner/.github/workflows/coverage.yml"
        )
        assert bazelrc_path is not None
        assert bazelversion_path is not None
        assert coverage_workflow_path is not None

        with open(bazelrc_path, encoding="utf-8") as bazelrc:
            cls.bazelrc_lines = {
                line.strip()
                for line in bazelrc
                if line.strip() and not line.lstrip().startswith("#")
            }
        with open(bazelversion_path, encoding="utf-8") as bazelversion:
            cls.bazel_version = bazelversion.read().strip()
        with open(coverage_workflow_path, encoding="utf-8") as coverage_workflow:
            cls.coverage_workflow = coverage_workflow.read()

    def test_pinned_bazel_supports_lost_input_rewinding(self):
        match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", self.bazel_version)
        self.assertIsNotNone(match, self.bazel_version)
        self.assertGreaterEqual(tuple(map(int, match.groups())), (8, 7, 0))

    def test_builds_rewind_evicted_remote_cache_inputs(self):
        self.assertIn("build --rewind_lost_inputs", self.bazelrc_lines)

    def test_whole_build_retry_is_bounded(self):
        self.assertIn(
            "build --experimental_remote_cache_eviction_retries=1",
            self.bazelrc_lines,
        )

    def test_remote_python_launchers_are_runfiles_aware(self):
        self.assertIn(
            "common --@rules_python//python/config_settings:bootstrap_impl=script",
            self.bazelrc_lines,
        )

    def test_coverage_skips_host_incompatible_explicit_targets(self):
        self.assertIn(
            "coverage --skip_incompatible_explicit_targets",
            self.bazelrc_lines,
        )

    def test_root_bazel_pin_uses_full_baseline_coverage_path(self):
        """Only root Bazel pins escalate to the main-push baseline.

        These are the files whose change bazel-diff structurally cannot model:
        it hashes the graph they define, so a pin moving underneath it is
        invisible to its impact analysis.

        `build_defs/*` and `.bazelrc` were on this list and are deliberately not
        any more. bazel-diff hashes .bzl files transitively, so they produce a
        real affected set - and escalating them composed with the coverage jobs'
        skip-on-full-fallback gate into zero coverage on the changes most able
        to move coverage. tools/coverage_lane_routing_tests.py holds the rest of
        that contract.
        """
        self.assertIn(
            "MODULE.bazel|.bazelversion|WORKSPACE|WORKSPACE.*|third_party/bazel/*)",
            self.coverage_workflow,
        )


if __name__ == "__main__":
    unittest.main()
