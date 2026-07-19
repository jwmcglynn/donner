import os
from pathlib import Path
import unittest


class GeodeTestSchedulingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.bazelrc = (runfiles / ".bazelrc").read_text()
        cls.rules = (runfiles / "build_defs/rules.bzl").read_text()

    def test_linux_suite_keeps_hardware_adapter_enabled(self):
        self.assertNotIn(
            "test:linux --test_env=DONNER_GEODE_FORCE_FALLBACK_ADAPTER=1",
            self.bazelrc,
        )

    def test_explicit_geode_suite_serializes_local_gpu_tests(self):
        self.assertIn("test:geode --local_test_jobs=1", self.bazelrc)

    def test_transitioned_geode_targets_serialize_when_local(self):
        self.assertIn('"exclusive-if-local"', self.rules)
        self.assertIn('renderer_backend == "geode"', self.rules)


if __name__ == "__main__":
    unittest.main()
