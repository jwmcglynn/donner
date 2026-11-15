import os
from pathlib import Path
import re
import subprocess
import textwrap
import unittest

from build_defs import check_local_gpu_tags


# The tag a GPU-opening test carries: one unit of the `local_gpu` resource.
LOCAL_GPU_TAG = '"resources:local_gpu:1"'
# The one declaration of the resource, applied to every command.
LOCAL_GPU_DECLARATION = "build --local_resources=local_gpu=1"
# Commands that may override the resource count, each with its reviewed reason. None may today.
REVIEWED_LOCAL_GPU_OVERRIDES = {}
# Packages that tag GPU-opening tests directly rather than through opens_gpu_device, with the
# number of tags each carries.
DIRECT_GPU_TAGS = {
    "donner/editor/tests/BUILD.bazel": 2,
    "donner/svg/renderer/geode/BUILD.bazel": 3,
}
# Bazel ignores resource tags when --local_test_jobs is set, so a command that sets it above 1
# must run no GPU-opening test. The browser GPU comparison runs browser tests only: it compares
# their results at 1 and at 4 concurrent tests.
PARALLEL_LOCAL_TEST_JOBS_TARGETS = {"//tools/ci:browser_gpu_comparison"}


def _logical_lines(text):
    """Lines of a file with backslash continuations joined."""
    lines, current = [], ""
    for line in text.splitlines():
        if line.rstrip().endswith("\\"):
            current += line.rstrip()[:-1] + " "
            continue
        lines.append(current + line)
        current = ""
    return lines + ([current] if current else [])


# --local_test_jobs in either the `=` or the space form. Bazel honours resource tags only at 0;
# 1 runs every local test serially. Any other value, a variable or HOST_CPUS included, is parallel.
_LOCAL_TEST_JOBS = re.compile(r"--local_test_jobs(?:=|\s+)([^\s\\]+)")
_SERIAL_OR_RESOURCE_BOUND = {"0", "1"}


def _parallel_local_test_jobs(line):
    return any(value.strip("'\"") not in _SERIAL_OR_RESOURCE_BOUND
               for value in _LOCAL_TEST_JOBS.findall(line))


def _rc_problems(bazelrc):
    """Problems of the .bazelrc declaration of the local GPU resource."""
    lines = [line for line in bazelrc.splitlines() if not line.lstrip().startswith("#")]
    problems = []
    declarations = [line for line in lines if "local_gpu" in line]
    if declarations != [LOCAL_GPU_DECLARATION]:
        problems.append(".bazelrc must declare exactly %r, found %r" % (
            LOCAL_GPU_DECLARATION, declarations))
    problems += [".bazelrc sets --local_test_jobs above 1: %s" % line
                 for line in lines if _parallel_local_test_jobs(line)]
    return problems


def _command_problems(path, line):
    """Problems of one logical workflow line."""
    problems = []
    if re.search(r"--(?:ignore_all_rc_files|noworkspace_rc)\b", line):
        problems.append("%s skips the .bazelrc that declares local_gpu: %s" % (path, line))
    if "local_gpu" in line and line not in REVIEWED_LOCAL_GPU_OVERRIDES:
        problems.append("%s overrides the local_gpu count: %s" % (path, line))
    targets = set(re.findall(r"(?<!\S)//\S+", line))
    parallel_ok = bool(targets) and targets <= PARALLEL_LOCAL_TEST_JOBS_TARGETS
    if _parallel_local_test_jobs(line) and not parallel_ok:
        problems.append("%s runs tests with --local_test_jobs above 1, which ignores the "
                        "local_gpu resource: %s" % (path, line))
    return problems


def local_gpu_problems(bazelrc, workflows):
    """Ways a command could run two GPU-opening tests at once on one machine."""
    problems = _rc_problems(bazelrc)
    for path, text in sorted(workflows.items()):
        problems += [problem for line in _logical_lines(text)
                     if not line.lstrip().startswith("#")
                     for problem in _command_problems(path, line.strip())]
    return problems


class GeodeTestSchedulingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.bazelrc = (runfiles / ".bazelrc").read_text()
        cls.rules = (runfiles / "build_defs/rules.bzl").read_text()
        cls.main_workflow = (runfiles / ".github/workflows/main.yml").read_text()
        github = runfiles / ".github"
        cls.workflows = {
            str(path.relative_to(runfiles)): path.read_text()
            for path in sorted(github.glob("workflows/*.y*ml")) + sorted(
                github.glob("actions/*/action.yml"))
        }
        cls.direct_tag_builds = {path: (runfiles / path).read_text() for path in DIRECT_GPU_TAGS}

    def test_linux_suite_keeps_hardware_adapter_enabled(self):
        self.assertNotIn(
            "test:linux --test_env=DONNER_GEODE_FORCE_FALLBACK_ADAPTER=1",
            self.bazelrc,
        )

    def test_explicit_geode_suite_serializes_local_gpu_tests(self):
        self.assertIn("test:geode --local_test_jobs=1", self.bazelrc)

    def test_gpu_opening_targets_take_the_local_gpu_resource(self):
        """GPU-opening tests share one `local_gpu` unit instead of the whole machine.

        `exclusive` and `exclusive-if-local` hold every other local test back, so
        the GPU-opening tests drained one at a time in a serial tail after the
        parallel phase. One unit of a declared resource still keeps two of them
        from running at once, but lets CPU-only tests run alongside.
        """
        self.assertIn("LOCAL_GPU_TEST_TAG = %s" % LOCAL_GPU_TAG, self.rules)
        self.assertIn("tags = tags + [LOCAL_GPU_TEST_TAG]", self.rules)
        self.assertIn('renderer_backend == "geode"', self.rules)
        self.assertNotIn('"exclusive-if-local"', self.rules)
        self.assertNotIn('"exclusive"', self.rules)

    def test_directly_tagged_gpu_tests_take_the_local_gpu_resource(self):
        # The Lint lane runs the same check over every tracked BUILD file.
        self.assertEqual([], check_local_gpu_tags.build_file_problems(self.direct_tag_builds))
        for path, expected in DIRECT_GPU_TAGS.items():
            with self.subTest(path=path):
                self.assertEqual(expected, self.direct_tag_builds[path].count(LOCAL_GPU_TAG))

    def test_build_file_check_rejects_whole_machine_and_multi_unit_tags(self):
        rule = 'cc_test(\n    name = "gpu_tests",\n    tags = [%s],\n)\n'
        cases = {
            '"exclusive-if-local"': "exclusive-if-local",
            '"exclusive"': "without a reviewed reason",
            '"resources:local_gpu:2"': "exactly one unit",
            '"resources:local_gpu:0.5"': "exactly one unit",
        }
        for tag, expected in cases.items():
            with self.subTest(tag=tag):
                problems = check_local_gpu_tags.build_file_problems(
                    {"donner/x/BUILD.bazel": rule % tag})
                self.assertEqual(1, len(problems), problems)
                self.assertIn(expected, problems[0])
                self.assertIn("donner/x/BUILD.bazel:3: gpu_tests", problems[0])
        reviewed = ('cc_test(\n    name = "dom_lifetime_perf_capture",\n'
                    '    tags = ["exclusive"],\n)\n')
        self.assertEqual([], check_local_gpu_tags.build_file_problems(
            {"donner/benchmarks/BUILD.bazel": reviewed}))
        self.assertEqual([], check_local_gpu_tags.build_file_problems(
            {"donner/x/BUILD.bazel": rule % LOCAL_GPU_TAG}))

    def test_every_command_runs_at_most_one_gpu_opening_test_at_a_time(self):
        self.assertIn(".github/workflows/main.yml", self.workflows)
        self.assertEqual([], local_gpu_problems(self.bazelrc, self.workflows))

    def test_local_gpu_policy_rejects_concurrent_gpu_tests(self):
        main = ".github/workflows/main.yml"
        command = "          bazelisk test --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS\n"
        cases = {
            "the declaration is removed": (
                self.bazelrc.replace(LOCAL_GPU_DECLARATION + "\n", "", 1), {},
                "must declare exactly"),
            "the declaration allows two": (
                self.bazelrc.replace(LOCAL_GPU_DECLARATION, "build --local_resources=local_gpu=2"),
                {}, "must declare exactly"),
            "a config raises the count": (
                self.bazelrc + "test:ci --local_resources=local_gpu=2\n", {},
                "must declare exactly"),
            "a config sets parallel local test jobs": (
                self.bazelrc + "test:ci --local_test_jobs=4\n", {}, "above 1"),
            "a config sets parallel local test jobs in the space form": (
                self.bazelrc + "test:ci --local_test_jobs HOST_CPUS\n", {}, "above 1"),
            "a lane sets parallel local test jobs in the space form": (self.bazelrc, {
                main: command.replace("$TARGETS", "--local_test_jobs 4 $TARGETS")},
                "ignores the local_gpu resource"),
            "a lane sets local test jobs from a variable": (self.bazelrc, {
                main: command.replace("$TARGETS", "--local_test_jobs=$JOBS $TARGETS")},
                "ignores the local_gpu resource"),
            "a lane sets local test jobs from the host CPU count": (self.bazelrc, {
                main: command.replace("$TARGETS", "--local_test_jobs=HOST_CPUS*.5 $TARGETS")},
                "ignores the local_gpu resource"),
            "a lane raises the count": (self.bazelrc, {main: command.replace(
                "$TARGETS", "--local_resources=local_gpu=2 $TARGETS")}, "overrides"),
            "a lane runs GPU tests with parallel local test jobs": (self.bazelrc, {
                main: command.replace("$TARGETS", "--local_test_jobs=4 $TARGETS")},
                "ignores the local_gpu resource"),
            "a lane runs GPU tests with parallel jobs on continued lines": (self.bazelrc, {
                main: "          bazelisk test --local_test_jobs=4 \\\n"
                      "            //tools/ci:browser_gpu_comparison //donner/...\n"},
                "ignores the local_gpu resource"),
            "a lane skips the repository rc": (self.bazelrc, {
                main: command.replace("bazelisk test", "bazelisk --ignore_all_rc_files test")},
                "skips the .bazelrc"),
        }
        for name, (bazelrc, workflows, expected) in cases.items():
            with self.subTest(case=name):
                problems = local_gpu_problems(bazelrc, workflows)
                self.assertTrue(any(expected in problem for problem in problems), problems)
        allowed = "          bazelisk test --local_test_jobs=4 //tools/ci:browser_gpu_comparison\n"
        self.assertEqual([], local_gpu_problems(self.bazelrc, {main: allowed}))
        # 0 lets local resources bound the tests, so the resource tag applies.
        for value in ("0", "1"):
            with self.subTest(local_test_jobs=value):
                self.assertEqual([], local_gpu_problems(
                    self.bazelrc + "test:ci --local_test_jobs=%s\n" % value,
                    {main: command.replace("$TARGETS", "--local_test_jobs %s $TARGETS" % value)}))

    def test_serialization_is_opt_in_not_implied_by_the_geode_backend(self):
        """The local GPU resource must be gated on opening a real GPU device.

        Applying it to every geode-BACKED target (which includes the
        `text_full` tier and every variant that merely links wgpu-native
        without touching it) would serialize the whole geode-configured test
        surface against itself, one target at a time, no matter how much
        executor capacity is available.
        """
        self.assertIn(
            'if renderer_backend == "geode" and opens_gpu_device:',
            self.rules,
            "the local GPU resource must be gated on opens_gpu_device, not on the "
            "geode backend alone",
        )
        self.assertIn("opens_gpu_device = False", self.rules)

    def test_remote_ci_can_parallelize_opted_in_gpu_tests_without_weakening_local_isolation(self):
        """Opted-in suites get a remote-only copy for the RE lane.

        The copy has the same transitioned executable, carries `no-local` and
        `ci-remote-gpu` instead of the local GPU tags, and is the only wrapper
        the RE lane selects for those suites. Local and hosted lanes must keep
        selecting the original wrapper, which takes the local GPU resource.
        """
        self.assertIn("remote_parallel_ci = False", self.rules)
        self.assertIn('name = name + "_ci_remote"', self.rules)
        self.assertIn('"ci-remote-gpu"', self.rules)
        self.assertIn('"no-local"', self.rules)
        self.assertIn('"local-gpu-isolated"', self.rules)
        self.assertIn('if tag not in [LOCAL_GPU_TEST_TAG, "local-gpu-isolated"]', self.rules)
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
        self.assertNotIn("mapfile -t TARGETS", remote_job)
        self.assertIn(
            'if ! mapped_targets="$(python3 tools/ci_remote_gpu_targets.py',
            remote_job,
        )
        test_step = remote_job.split('      - name: Test\n', 1)[1].split('\n      - ', 1)[0]
        self.assertEqual(2, test_step.count('=~ ^[[:space:]]*$ ]]; then'))

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
