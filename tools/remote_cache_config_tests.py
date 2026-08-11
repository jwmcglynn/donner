import re
import unittest

from python.runfiles import runfiles


class RemoteCacheConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        bazelrc_path = resolver.Rlocation("donner/.bazelrc")
        bazelversion_path = resolver.Rlocation("donner/.bazelversion")
        assert bazelrc_path is not None
        assert bazelversion_path is not None

        with open(bazelrc_path, encoding="utf-8") as bazelrc:
            cls.bazelrc_lines = {
                line.strip()
                for line in bazelrc
                if line.strip() and not line.lstrip().startswith("#")
            }
        with open(bazelversion_path, encoding="utf-8") as bazelversion:
            cls.bazel_version = bazelversion.read().strip()

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


if __name__ == "__main__":
    unittest.main()
