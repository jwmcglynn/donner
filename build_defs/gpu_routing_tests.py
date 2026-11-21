"""Tests for build_defs/check_gpu_routing.py: each rule passes the tree and fails its mutation."""

import ast
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
        platforms = ast.parse(self.tree[routing.PLATFORMS_BUILD])
        definitions = [node for node in ast.walk(platforms) if isinstance(node, ast.Call)
                       and getattr(node.func, "id", None) == "platform"]
        attributes = [{kw.arg: kw.value for kw in call.keywords}
                      for call in definitions]
        gpu, = [attrs for attrs in attributes if getattr(attrs.get("name"), "value", None)
                == "linux_ci_gpu"]
        self.assertEqual(ast.literal_eval(gpu["parents"]), [":linux_aarch64_ci_base"])
        base, = [attrs for attrs in attributes if getattr(attrs.get("name"), "value", None)
                 == "linux_aarch64_ci_base"]
        self.assertNotIn("donner-toolchain", ast.literal_eval(base["exec_properties"]))
        self.assertNotIn(":worker_llvm", ast.literal_eval(base["constraint_values"]))
        self.assertEqual(ast.literal_eval(gpu["exec_properties"]), {"gpu": "present"})
        self.assertNotIn("constraint_values", gpu)

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

    def test_only_the_native_test_registration_can_select_the_platform(self):
        registration = "build_defs/ci_linux_gpu.bzl"
        platform = '"//tools/ci:linux_ci_gpu"'
        invocation = "ci_platform_test(name=name, target_platform=%s)" % platform
        self.assert_clean({registration: invocation})
        for path, source in [
            ("other.bzl", invocation),
            (registration, invocation.replace("ci_platform_test", "ci_platform_build")),
            (registration, invocation.replace("target_platform=", "dep=")),
            (registration, "some_call(%s)" % platform),
        ]:
            with self.subTest(path=path, source=source):
                self.assert_problem({path: source}, path, "named outside its definition")

    def test_command_cannot_make_the_linux_gpu_platform_global(self):
        option = "--platforms=//tools/ci:linux_ci_gpu"
        for path, text in [(".bazelrc", "build:ci-platforms " + option),
                           ("examples/.bazelrc", "test " + option),
                           (".github/workflows/main.yml", "    run: bazel test " + option + " //..."),
                           ("ci/test.sh", "bazel test " + option + " //..."),
                           ("tools/ci/lanes.py", "OPTIONS = [%r]" % option)]:
            with self.subTest(path=path):
                self.assert_problem({path: text}, path, "only native test registration")

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
