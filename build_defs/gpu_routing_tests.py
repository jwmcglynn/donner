"""Tests for build_defs/check_gpu_routing.py: each rule passes the tree and fails its mutation."""

from pathlib import Path
import unittest

from python.runfiles import runfiles

from build_defs import check_gpu_routing as routing

# Real files that route tests to executors today. The tree test checks only these copies; the
# Lint lane runs the check over every tracked file.
_TREE = (
    ".bazelrc",
    ".github/workflows/main.yml",
    "build_defs/rules.bzl",
    "donner/editor/tests/BUILD.bazel",
    "donner/svg/renderer/geode/BUILD.bazel",
    "tools/ci/BUILD.bazel",
    "tools/ci/lanes.py",
)

_TEST = """\
donner_multi_transitioned_test(
    name = "geode_example_tests",
    dep = ":geode_example_tests_impl",
    opens_gpu_device = True,
    renderer_backend = "geode",
    PROPERTY
)
"""
_PLATFORM_MESSAGE = "outside the reviewed GPU platforms"
_CONFIG_RC = "build:ci-linux-gpu --platforms=//tools/ci:linux_ci_gpu"


def _read(path):
    return Path(runfiles.Create().Rlocation("donner/" + path)).read_text(encoding="utf-8")


class GpuRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tree = {path: _read(path) for path in _TREE}

    def assert_problem(self, files, path, reason):
        found = routing.problems(files)
        self.assertTrue([problem for problem in found
                         if problem.startswith(path + ":") and reason in problem], found)

    def assert_clean(self, files):
        self.assertEqual([], routing.problems(files))

    def test_tree_passes(self):
        self.assert_clean(self.tree)
        # The exemptions are exercised: both reviewed platforms carry the property in the real
        # BUILD file, and the config's one platform line is in the real rc file.
        platforms = self.tree[routing.PLATFORMS_BUILD]
        for name in routing.REVIEWED_GPU_PLATFORMS:
            with self.subTest(platform=name):
                self.assertIn('name = "%s"' % name, platforms)
        self.assertIn(routing.LINUX_GPU_CONFIG_LINE, self.tree[".bazelrc"].splitlines())

    def test_gpu_property_spellings_on_a_target_are_rejected(self):
        path = "donner/svg/renderer/geode/BUILD.bazel"
        for spelling in ('exec_properties = {"gpu": "present"},',
                         'exec_properties = {"test.gpu": "present"},',
                         'exec_properties = dict(gpu = "present"),'):
            with self.subTest(spelling=spelling):
                self.assert_problem({path: _TEST.replace("PROPERTY", spelling)}, path,
                                    _PLATFORM_MESSAGE)
        constant = 'KEY = "gpu"\n' + _TEST.replace("PROPERTY", 'exec_properties = {KEY: "1"},')
        self.assert_problem({path: constant}, path, _PLATFORM_MESSAGE)

    def test_gpu_property_in_a_macro_is_rejected(self):
        path = "build_defs/rules.bzl"
        mutated = 'def gpu_test(**kwargs):\n    kwargs["exec_properties"] = {"gpu": "present"}\n'
        self.assert_problem({path: mutated}, path, _PLATFORM_MESSAGE)

    def test_gpu_property_on_an_unreviewed_platform_is_rejected(self):
        path = routing.PLATFORMS_BUILD
        # One line, right after a reviewed platform: the name is read from this call, not from a
        # preceding line.
        mutated = self.tree[path] + (
            '\nplatform(name = "linux_ci_other", exec_properties = {"gpu": "present"})\n')
        self.assert_problem({path: mutated}, path, _PLATFORM_MESSAGE)
        elsewhere = "tools/other/BUILD.bazel"
        moved = 'platform(\n    name = "remote_macos_aarch64_gpu",\n' \
                '    exec_properties = {"gpu": "present"},\n)\n'
        self.assert_problem({elsewhere: moved}, elsewhere, _PLATFORM_MESSAGE)

    def test_child_of_a_gpu_platform_is_rejected(self):
        path = routing.PLATFORMS_BUILD
        for parent in ("linux_ci_gpu", "remote_macos_aarch64_gpu"):
            with self.subTest(parent=parent):
                mutated = self.tree[path] + (
                    '\nplatform(\n    name = "child",\n    parents = [":%s"],\n)\n' % parent)
                self.assert_problem({path: mutated}, path, "inherits a reviewed GPU platform")

    def test_linux_gpu_platform_as_a_transition_target_is_rejected(self):
        path = "donner/editor/tests/BUILD.bazel"
        mutated = self.tree[path] + (
            '\nci_platform_test(\n    name = "ci_linux_gpu_example",\n'
            '    dep = ":example",\n    target_platform = "//tools/ci:linux_ci_gpu",\n)\n')
        self.assert_problem({path: mutated}, path, "named outside its definition")

    def test_module_registration_of_a_pattern_or_gpu_platform_is_rejected(self):
        for value in ("//tools/ci:all", "//tools/ci/...", "//tools/ci:remote_macos_aarch64_gpu"):
            with self.subTest(value=value):
                mutated = 'register_execution_platforms("%s")\n' % value
                self.assert_problem({"MODULE.bazel": mutated}, "MODULE.bazel",
                                    "registers a GPU platform or a target pattern")

    def test_unparseable_starlark_is_rejected(self):
        self.assert_problem({"a/BUILD.bazel": "platform(\n"}, "a/BUILD.bazel", "cannot be parsed")

    def test_default_gpu_property_spellings_are_rejected(self):
        for line in ("build --remote_default_exec_properties=gpu=present",
                     'build --remote_default_exec_properties="gpu=present"',
                     "build --remote_default_exec_properties gpu=present",
                     'build --remote_default_platform_properties=properties:{name:"gpu" '
                     'value:"present"}'):
            with self.subTest(line=line):
                self.assert_problem({".bazelrc": self.tree[".bazelrc"] + line + "\n"}, ".bazelrc",
                                    "default `gpu` execution property")
        workflow = ".github/workflows/main.yml"
        self.assert_problem(
            {workflow: self.tree[workflow] + "\n      run: bazel build "
                                             "--remote_default_exec_properties=gpu=present //x\n"},
            workflow, "default `gpu` execution property")

    def test_platform_flags_reaching_a_gpu_platform_are_rejected(self):
        for line in ("build --extra_execution_platforms=//tools/ci:all",
                     "build --extra_execution_platforms=//tools/ci:linux_ci,//tools/ci:*",
                     "build --host_platform=//tools/ci:remote_macos_aarch64_gpu",
                     "build --extra_execution_platforms //tools/ci/..."):
            with self.subTest(line=line):
                self.assert_problem({".bazelrc": self.tree[".bazelrc"] + line + "\n"}, ".bazelrc",
                                    "names a GPU platform or a target pattern")

    def test_rc_inclusion_of_the_config_is_rejected(self):
        for line in ("build:ci --config=ci-linux-gpu", "test --config=ci-linux-gpu",
                     "common:lane --config=ci-linux-gpu", "build --config ci-linux-gpu"):
            with self.subTest(line=line):
                self.assert_problem({".bazelrc": self.tree[".bazelrc"] + line + "\n"}, ".bazelrc",
                                    "includes `--config=ci-linux-gpu`")
        other = "examples/.bazelrc"
        self.assert_problem({other: "test --config=ci-linux-gpu\n"}, other,
                            "includes `--config=ci-linux-gpu`")

    def test_linux_gpu_platform_outside_its_config_line_is_rejected(self):
        rc = self.tree[".bazelrc"]
        for line in ("build:ci-linux-gpu --extra_execution_platforms=//tools/ci:linux_ci_gpu",
                     "build:ci --platforms=//tools/ci:linux_ci_gpu",
                     "test --platforms=//tools/ci:linux_ci_gpu"):
            with self.subTest(line=line):
                self.assert_problem({".bazelrc": rc + line + "\n"}, ".bazelrc",
                                    "may only be the target platform")
        self.assert_problem({"examples/.bazelrc": _CONFIG_RC + "\n"}, "examples/.bazelrc",
                            "may only be the target platform")

    def test_command_using_the_config_needs_the_instance(self):
        workflow = ".github/workflows/main.yml"
        for command in ("bazelisk test --config=ci --config=ci-linux-gpu //x",
                        "bazel coverage --config=ci-linux-gpu //x",
                        "bazelisk --output_base=/o test --config=ci-linux-gpu \\\n  //x",
                        "cd x && bazelisk test --config=ci-linux-gpu //x"):
            with self.subTest(command=command):
                self.assert_problem({workflow: "      run: %s\n" % command}, workflow,
                                    "without `--remote_instance_name`")
        script = "tools/ci/run_gpu.sh"
        self.assert_problem({script: 'tools/ci/bazel_watchdog.sh log "$BAZEL" '
                                     "--config=ci-linux-gpu //x\n"}, script,
                            "Bazel verb cannot be read")
        for command in ("bazelisk build --config=ci --config=ci-linux-gpu //x",
                        "bazelisk test --config=ci --config=ci-linux-gpu \\\n"
                        "  --remote_instance_name=\"$GPU_INSTANCE\" //x"):
            with self.subTest(command=command):
                self.assert_clean({workflow: "      run: %s\n" % command})

    def test_lane_python_naming_the_config_or_platform_is_rejected(self):
        path = "tools/ci/lanes.py"
        for literal in ('"--config=ci-linux-gpu"', '"--platforms=//tools/ci:linux_ci_gpu"'):
            with self.subTest(literal=literal):
                mutated = self.tree[path] + "\nGPU = (%s,)\n" % literal
                self.assert_problem({path: mutated}, path, "names the Linux GPU config or platform")
        documented = '"""Runs tests with --config=ci-linux-gpu."""\n'
        self.assert_clean({"tools/ci/gpu_lane.py": documented})

    def test_full_check_reads_routing_files_only(self):
        for path in (".bazelrc", "examples/.bazelrc", "BUILD.bazel", "a/BUILD", "a/b.bzl",
                     "MODULE.bazel", "WORKSPACE", "tools/ci/lanes.py", "tools/ci/lane.sh",
                     ".github/workflows/main.yml", ".github/actions/pr-trust/action.yml",
                     ".github/actions/pr-trust/classify.sh"):
            with self.subTest(path=path):
                self.assertTrue(routing.checked_path(path))
        for path in ("docs/building.md", "tools/lint.py", "build_defs/x.py",
                     "third_party/x/run.sh"):
            with self.subTest(path=path):
                self.assertFalse(routing.checked_path(path))


if __name__ == "__main__":
    unittest.main()
