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

    def test_serialization_is_opt_in_not_implied_by_the_geode_backend(self):
        """`exclusive-if-local` must be gated on opening a real GPU device.

        Applying it to every geode-BACKED target (which includes the
        `text_full` tier and every variant that merely links wgpu-native
        without touching it) puts the whole geode-configured test surface into
        Bazel's single-file exclusive tail, so the test phase drains one target
        at a time no matter how much executor capacity is available.
        """
        self.assertIn(
            'if renderer_backend == "geode" and opens_gpu_device:',
            self.rules,
            "exclusive-if-local must be gated on opens_gpu_device, not on the "
            "geode backend alone",
        )
        self.assertIn("opens_gpu_device = False", self.rules)

    def test_variant_specs_cannot_pin_a_remote_execution_platform_property(self):
        """Variant specs must not carry `exec_properties`.

        A remote-execution platform property is a HARD match: when no worker
        in the pool advertises it, the action sits in the scheduler queue
        forever. The per-test timeout does not apply, because it only starts
        once a test RUNS, so there is no bound at all. A `gpu` property on the
        geode render suites parked every self-hosted run in the queue until the
        job-level timeout killed it (measured: ~200 minutes of a 210-minute
        budget, on three separate runs, with zero of the queued tests ever
        starting). Keep the forwarding hook out of the macro so a spec cannot
        reintroduce it without a deliberate rule change.
        """
        self.assertNotIn("exec_properties", self.rules)


if __name__ == "__main__":
    unittest.main()
