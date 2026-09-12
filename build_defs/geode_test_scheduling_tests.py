import os
from pathlib import Path
import subprocess
import textwrap
import unittest


class GeodeTestSchedulingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.bazelrc = (runfiles / ".bazelrc").read_text()
        cls.rules = (runfiles / "build_defs/rules.bzl").read_text()
        cls.main_workflow = (runfiles / ".github/workflows/main.yml").read_text()
        cls.coverage_workflow = (runfiles / ".github/workflows/coverage.yml").read_text()

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

    def test_remote_ci_can_parallelize_opted_in_gpu_tests_without_weakening_local_isolation(self):
        """Remote copies bypass Bazel's pre-spawn exclusive queue only in the RE lane.

        Bazel 8.7 partitions `exclusive-if-local` targets into its exclusive
        top-level queue before the TestRunner spawn selects a remote strategy.
        The inner spawn is remote, but the shards still drain one at a time.
        Opted-in tests therefore need a separately tagged wrapper with the same
        transitioned executable and no exclusive tag. Local and hosted lanes
        must keep selecting the original isolated wrapper.
        """
        self.assertIn("remote_parallel_ci = False", self.rules)
        self.assertIn('name = name + "_ci_remote"', self.rules)
        self.assertIn('"ci-remote-gpu"', self.rules)
        self.assertIn('"no-local"', self.rules)
        self.assertIn('"local-gpu-isolated"', self.rules)
        self.assertIn(
            "test --test_tag_filters=-ci-remote-gpu",
            self.bazelrc,
        )
        remote_job = self.main_workflow.split("  linux-self-hosted:\n", 1)[1]
        remote_job = remote_job.split("\n  macos:\n", 1)[0]
        self.assertIn("--strategy=TestRunner=remote", remote_job)
        self.assertIn("--remote_local_fallback=false", remote_job)
        self.assertIn(
            "--test_tag_filters=-manual,-perf,-local-gpu-isolated",
            remote_job,
        )
        self.assertIn("--strategy=TestRunner=remote", self.coverage_workflow)
        self.assertIn("--remote_local_fallback=false", self.coverage_workflow)
        self.assertIn("-local-gpu-isolated", self.coverage_workflow)
        self.assertNotIn("mapfile -t TARGETS", remote_job)
        self.assertNotIn("mapfile -t TARGETS", self.coverage_workflow)
        self.assertIn(
            'if ! mapped_targets="$(python3 tools/ci_remote_gpu_targets.py',
            remote_job,
        )
        self.assertIn(
            'if ! mapped_targets="$(python3 tools/ci_remote_gpu_targets.py',
            self.coverage_workflow,
        )
        self.assertEqual(2, remote_job.count('if [[ -z "${'))
        self.assertIn('if [[ -z "${mapped_targets// /}" ]]; then', self.coverage_workflow)

    def test_remote_target_mapping_blocks_propagate_failure_and_empty_output(self):
        remote_job = self.main_workflow.split("  linux-self-hosted:\n", 1)[1]
        remote_job = remote_job.split("\n  macos:\n", 1)[0]
        cases = [
            (
                "test",
                remote_job,
                'TARGETS="$mapped_targets"',
                'TARGETS="//donner/editor/tests:rnr_replay_tests_geode"',
                'printf "%s\\n" "$TARGETS"',
            ),
            (
                "coverage",
                self.coverage_workflow,
                'read -r -a TARGETS <<< "$mapped_targets"',
                'TARGETS=("//donner/editor/tests:rnr_replay_tests_geode")',
                'printf "%s\\n" "${TARGETS[@]}"',
            ),
        ]
        fake_mapper = r"""
        python3() {
          case "$MAPPER_MODE" in
            fail) return 23 ;;
            empty) return 0 ;;
            ok) printf '%s\n' "$MAPPED_TARGETS" ;;
          esac
        }
        """
        expected = "//donner/editor/tests:rnr_replay_tests_geode_ci_remote"
        for name, workflow, final_line, setup, print_line in cases:
            start = workflow.index('          if ! mapped_targets="$(python3')
            end = workflow.index(final_line, start) + len(final_line)
            block = textwrap.dedent(workflow[start:end])
            script = "set -euo pipefail\n" + textwrap.dedent(fake_mapper) + setup + "\n" + block
            for mode in ("fail", "empty"):
                with self.subTest(route=name, mode=mode):
                    result = subprocess.run(
                        ["bash", "-c", script],
                        check=False,
                        capture_output=True,
                        text=True,
                        env={**os.environ, "MAPPER_MODE": mode, "MAPPED_TARGETS": expected},
                    )
                    self.assertEqual(1, result.returncode, result.stdout + result.stderr)
            with self.subTest(route=name, mode="ok"):
                result = subprocess.run(
                    ["bash", "-c", script + "\n" + print_line],
                    check=False,
                    capture_output=True,
                    text=True,
                    env={**os.environ, "MAPPER_MODE": "ok", "MAPPED_TARGETS": expected},
                )
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertEqual(expected, result.stdout.strip())

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
