import os
from pathlib import Path
import re
import subprocess
import textwrap
import unittest

from build_defs import check_local_gpu_tags
from tools.ci import lane
from tools.ci import lane_view
from tools.ci import lanes


# The tag a GPU-opening test carries: one unit of the `local_gpu` resource.
LOCAL_GPU_TAG = '"resources:local_gpu:1"'
# The one declaration of the resource, applied to every command.
LOCAL_GPU_DECLARATION = "build --local_resources=local_gpu=1"
# .bazelrc or workflow lines that may raise the resource count, each with its reviewed reason.
REVIEWED_LOCAL_GPU_OVERRIDES = {
    # Owner decision, 2026-11-14. One unit exists because a hardware adapter is process-global
    # and one device loss could cascade into other GPU tests. Four concurrent GPU tests accept
    # that risk on the main.yml macOS lanes to shorten the GPU part of the test step; a device
    # loss would show as several GPU tests failing together. Each of those lanes passes
    # --config=ci-macos (//tools:macos_cache_key_parity_tests); developer runs, other macOS jobs
    # that do not pass the config and every other client keep one unit.
    "build:ci-macos --local_resources=local_gpu=4":
        "the main.yml macOS lanes run up to four GPU-opening tests at a time",
}
# The config that carries the macOS CI override. Only jobs on macOS runners may pass it.
MACOS_CI_CONFIG = "ci-macos"
_MACOS_RUNNER = re.compile(r"macOS|macos|xcode-")
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
    declarations = [line.strip() for line in lines if "local_gpu" in line]
    if declarations.count(LOCAL_GPU_DECLARATION) != 1:
        problems.append(".bazelrc must declare exactly %r, found %r" % (
            LOCAL_GPU_DECLARATION, declarations))
    problems += [".bazelrc sets local_gpu without a reviewed reason: %s" % line
                 for line in declarations
                 if line != LOCAL_GPU_DECLARATION and line not in REVIEWED_LOCAL_GPU_OVERRIDES]
    problems += [".bazelrc must carry the reviewed local_gpu override %r once" % line
                 for line in REVIEWED_LOCAL_GPU_OVERRIDES if declarations.count(line) != 1]
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


def _macos_config_problems(path, text):
    """Jobs that pass the macOS CI config, and with it four local_gpu units, off macOS.

    YAML comment lines are ignored, so a job may describe the config without passing it. Not
    covered: a workflow-level env variable, a composite action or another config that composes
    the macOS CI config."""
    if "\njobs:\n" not in text:
        return []
    body = "\n".join(line for line in text.split("\njobs:\n", 1)[1].splitlines()
                     if not line.lstrip().startswith("#")) + "\n"
    problems = []
    for match in re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\n((?:(?:    .*|[ \t]*)\n)*)", body):
        job = match.group(2)
        runs_on = re.search(r"(?m)^    runs-on: (.+)$", job)
        if ("--config=%s" % MACOS_CI_CONFIG in job and
                not (runs_on and _MACOS_RUNNER.search(runs_on.group(1)))):
            problems.append("%s job %s passes --config=%s off a macOS runner" % (
                path, match.group(1), MACOS_CI_CONFIG))
    return problems


def local_gpu_problems(bazelrc, workflows):
    """Ways a command could run more GPU-opening tests at once than its machine allows."""
    problems = _rc_problems(bazelrc)
    for path, text in sorted(workflows.items()):
        problems += [problem for line in _logical_lines(text)
                     if not line.lstrip().startswith("#")
                     for problem in _command_problems(path, line.strip())]
        problems += _macos_config_problems(path, text)
    return problems


class GeodeTestSchedulingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.bazelrc = (runfiles / ".bazelrc").read_text()
        cls.rules = (runfiles / "build_defs/rules.bzl").read_text()
        cls.main_workflow = (runfiles / ".github/workflows/main.yml").read_text()
        github = runfiles / ".github"
        # Each lane step with its commands from the lane table (tools/ci/lane_view.py).
        cls.workflow_paths = sorted(github.glob("workflows/*.y*ml")) + sorted(
            github.glob("actions/*/action.yml"))
        cls.workflows = cls._workflow_texts(runfiles)
        cls.direct_tag_builds = {path: (runfiles / path).read_text() for path in DIRECT_GPU_TAGS}

    def test_linux_suite_keeps_hardware_adapter_enabled(self):
        self.assertNotIn(
            "test:linux --test_env=DONNER_GEODE_FORCE_FALLBACK_ADAPTER=1",
            self.bazelrc,
        )

    def test_explicit_geode_suite_serializes_local_gpu_tests(self):
        self.assertIn("test:geode --local_test_jobs=1", self.bazelrc)

    def test_gpu_opening_targets_take_the_local_gpu_resource(self):
        """GPU-opening tests take a `local_gpu` unit instead of the whole machine.

        `exclusive` and `exclusive-if-local` hold every other local test back, so
        the GPU-opening tests drained one at a time in a serial tail after the
        parallel phase. Each test takes one unit of a declared resource, so the
        machine's unit count (one by default, four on the main.yml macOS lanes) bounds
        how many run at once, and CPU-only tests run alongside.
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

    @classmethod
    def _workflow_texts(cls, runfiles):
        return {str(path.relative_to(runfiles)): lane_view.with_dry_runs(path.read_text())
                for path in cls.workflow_paths}

    def test_every_command_runs_at_most_one_gpu_opening_test_at_a_time(self):
        self.assertIn(".github/workflows/main.yml", self.workflows)
        self.assertIn("--local_test_jobs=1", self.workflows[".github/workflows/main.yml"])
        self.assertEqual([], local_gpu_problems(self.bazelrc, self.workflows))

    def test_local_gpu_policy_reads_the_lane_table(self):
        # The lane phases' commands live in tools/ci/lanes.py; an edit there must fail like an
        # edit to a workflow step.
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        for edit, expected in (
                (lane_view.replacing("--local_test_jobs=1", "--local_test_jobs=2"),
                 "ignores the local_gpu resource"),
                (lane_view.replacing("--local_test_jobs=1", "--local_test_jobs=1",
                                     "--local_resources=local_gpu=2"), "overrides")):
            with self.subTest(expected=expected):
                with lane_view.edited_lane("macos-self-hosted", edit, ("browser_test",)):
                    problems = local_gpu_problems(self.bazelrc, self._workflow_texts(runfiles))
                self.assertTrue(any(expected in problem for problem in problems), problems)

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
                "without a reviewed reason"),
            "macOS CI raises the count to five": (
                self.bazelrc.replace("build:ci-macos --local_resources=local_gpu=4",
                                     "build:ci-macos --local_resources=local_gpu=5"), {},
                "without a reviewed reason"),
            "Linux raises the count to four": (
                self.bazelrc + "build:linux --local_resources=local_gpu=4\n", {},
                "without a reviewed reason"),
            "a Linux job passes the macOS CI config": (self.bazelrc, {
                main: "on: push\njobs:\n  linux:\n    runs-on: ubuntu-24.04\n    steps:\n"
                      "      - run: bazelisk test --config=ci --config=ci-macos //...\n"},
                "passes --config=ci-macos off a macOS runner"),
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
        macos_job = ("on: push\njobs:\n  macos:\n    runs-on: [self-hosted, macOS, ARM64]\n"
                     "    steps:\n"
                     "      - run: bazelisk test --config=ci --config=ci-macos //...\n")
        self.assertEqual([], local_gpu_problems(self.bazelrc, {main: macos_job}))
        # A Linux job that only mentions the config in a comment does not pass it.
        commented = ("on: push\njobs:\n  launcher:\n    runs-on: ubuntu-24.04\n"
                     "    # The remote macOS lane it starts passes --config=ci-macos.\n"
                     "    steps:\n"
                     "      # --config=ci-macos is applied on the macOS side.\n"
                     "      - run: bazelisk test --config=ci //...\n")
        self.assertEqual([], local_gpu_problems(self.bazelrc, {main: commented}))
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
        # The self-hosted Linux lane's test phase (tools/ci/lanes.py): it rejects an empty
        # selection, maps each opted-in suite to its remote wrapper (failing closed on a failed or
        # empty mapping, //tools/ci:lane_tests), then tests the mapped targets remotely.
        self.assertIn("run: python3 tools/ci/lane.py phase linux-self-hosted test",
                      self.main_workflow)
        phase = lanes.LANES["linux-self-hosted"].phases["test"]
        self.assertEqual([lanes.RequireSelection, lanes.MapRemoteGpu, lanes.Bazel],
                         [type(action) for action in phase.actions])
        [command] = [action["argv"] for action in
                     lane.dry_run("linux-self-hosted", "test", {})["actions"]
                     if action["action"] == "bazel"]
        self.assertIn("--strategy=TestRunner=remote", command)
        self.assertIn("--remote_local_fallback=false", command)
        self.assertIn("--test_tag_filters=-manual,-perf,-local-gpu-isolated,-fuzz_soak", command)
        self.assertEqual("$(python3 tools/ci_remote_gpu_targets.py @selected_targets)",
                         command[-1])

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
