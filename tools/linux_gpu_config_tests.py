"""Keeps `--config=ci-linux-gpu` a top-level GPU target platform over the mixed CI configuration.

The config runs Linux tests on a remote execution instance with a hardware GPU. It must build
exactly as `--config=ci-platforms`, so every build action keeps its key and cache entry, and differ
only in what the test actions declare: the Linux GPU target platform, which asks for a GPU, the
driver environment that the hardware run really uses, and the requirement that Donner's Vulkan
device selection refuse a software device. It must never name the remote instance or
any other private runner configuration, because the instance belongs only on the command line of
the test invocation; an rc config that named it would send every build of that config to the GPU
executor.
"""

import ast
from pathlib import Path
import unittest

from python.runfiles import runfiles
from tools import bazelrc_configs


CONFIG = "ci-linux-gpu"
BASE = "ci-platforms"
CI_BUILD = "tools/ci/BUILD.bazel"
DOCS = "docs/building.md"
BASE_PLATFORM = "linux_ci"
GPU_PLATFORM = "linux_ci_gpu"
# The test environment the config declares, after any rc file's, so the test key states the
# hardware run: the driver variables a software-rendering environment may set, and the hardware
# adapter requirement.
TEST_ENVIRONMENT = {
    "VK_DRIVER_FILES": "/usr/share/vulkan/icd.d",
    "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d",
    "VK_LOADER_DRIVERS_DISABLE": "*lvp*",
    "XDG_RUNTIME_DIR": "/tmp",
    # Donner's Vulkan device selection refuses a CPU-type device, for loaders that ignore the
    # driver filter.
    "DONNER_REQUIRE_HARDWARE_GPU": "1",
}


def _read(path):
    return Path(runfiles.Create().Rlocation("donner/%s" % path)).read_text(encoding="utf-8")


def _expand(command, config, rc=None):
    return bazelrc_configs.expand(command, ["--config=" + config],
                                  bazelrc_configs.parse(_read(".bazelrc") if rc is None else rc))


def _platforms(text):
    """{name: {attribute: literal}} for each top-level `platform(...)` call in a BUILD file."""
    calls = {}
    for node in ast.parse(text).body:
        if (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
                and getattr(node.value.func, "id", None) == "platform"):
            attributes = {keyword.arg: ast.literal_eval(keyword.value)
                          for keyword in node.value.keywords}
            calls[attributes["name"]] = attributes
    return calls


def config_problems(command, rc):
    """Why `bazel <command> --config=ci-linux-gpu` would not be the GPU variant of ci-platforms."""
    base = _expand(command, BASE, rc)
    options = _expand(command, CONFIG, rc)
    problems = ["private runner configuration: %s" % option
                for option in bazelrc_configs.private_runner_flags(options)]
    if options[:len(base)] != base:
        problems.append("does not start with the flags of --config=%s" % BASE)
    added = options[len(base):]
    if bazelrc_configs.last_value(options, "platforms") != "//tools/ci:" + GPU_PLATFORM:
        problems.append("does not make the Linux GPU platform the target platform")
    environment = {}
    for option in added:
        if option.startswith("--test_env="):
            name, _, value = option.removeprefix("--test_env=").partition("=")
            environment[name] = value
    if environment != TEST_ENVIRONMENT:
        problems.append("declares the test environment %r, not %r" % (environment,
                                                                      TEST_ENVIRONMENT))
    expected = ["--platforms=//tools/ci:" + GPU_PLATFORM] + [
        "--test_env=%s=%s" % item for item in TEST_ENVIRONMENT.items()]
    if command == "test":
        expected.append("--nocache_test_results")
    if sorted(added) != sorted(expected):
        problems.append("adds %r instead of %r" % (added, expected))
    return problems


def platform_problems(platforms):
    """Why the Linux GPU platform would not be the Linux CI platform asking for a GPU."""
    gpu = platforms.get(GPU_PLATFORM)
    if gpu is None:
        return ["%s is not defined" % GPU_PLATFORM]
    problems = []
    if gpu.get("parents") != [":" + BASE_PLATFORM]:
        problems.append("%s does not inherit %s" % (GPU_PLATFORM, BASE_PLATFORM))
    if gpu.get("exec_properties") != {"gpu": "present"}:
        problems.append("%s adds %r, not only the GPU request" % (GPU_PLATFORM,
                                                                  gpu.get("exec_properties")))
    if "constraint_values" in gpu:
        # Equal constraints keep constraint-based select() and target compatibility identical to
        # the base. A config_setting on the --platforms value itself does not match.
        problems.append("%s changes the constraints of %s" % (GPU_PLATFORM, BASE_PLATFORM))
    return problems


class LinuxGpuConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rc = _read(".bazelrc")

    def test_config_is_the_gpu_variant_of_the_mixed_ci_config(self):
        for command in ("build", "test"):
            with self.subTest(command=command):
                self.assertEqual([], config_problems(command, self.rc))

    def test_build_and_test_share_every_build_flag(self):
        # The build invocation and the test invocation must configure the same actions, so the
        # second one finds every build action in the cache.
        build, test = (_expand(command, CONFIG) for command in ("build", "test"))
        self.assertEqual(build, [option for option in test
                                 if option not in ("--nocache_test_results", "--test_tag_filters=")])

    def test_config_naming_the_instance_or_executor_is_rejected(self):
        for line in ("build:ci-linux-gpu --remote_instance_name=example",
                     "test:ci-linux-gpu --remote_executor=grpcs://executor.example:443",
                     "build:ci-platforms --remote_instance_name=example",
                     "build:ci-linux-gpu --jobs=3"):
            with self.subTest(line=line):
                problems = config_problems("test", self.rc + line + "\n")
                self.assertTrue([problem for problem in problems
                                 if problem.startswith("private runner configuration")], problems)

    def test_config_that_inherits_the_driver_environment_is_rejected(self):
        line = "build:ci-linux-gpu --test_env=VK_DRIVER_FILES=/usr/share/vulkan/icd.d\n"
        self.assertIn(line, self.rc)
        self.assertTrue(config_problems("test", self.rc.replace(line, "")))
        self.assertTrue(config_problems("test", self.rc + "build:ci-linux-gpu --test_env=VK_X=1\n"))
        requirement = "build:ci-linux-gpu --test_env=DONNER_REQUIRE_HARDWARE_GPU=1\n"
        self.assertIn(requirement, self.rc)
        self.assertTrue(config_problems("test", self.rc.replace(requirement, "")))

    def test_config_that_caches_hardware_results_is_rejected(self):
        line = "test:ci-linux-gpu --nocache_test_results\n"
        self.assertIn(line, self.rc)
        self.assertTrue(config_problems("test", self.rc.replace(line, "")))

    def test_config_that_leaves_the_mixed_platform_is_rejected(self):
        line = "build:ci-linux-gpu --platforms=//tools/ci:linux_ci_gpu\n"
        self.assertIn(line, self.rc)
        self.assertTrue(config_problems("build", self.rc.replace(line, "")))
        self.assertTrue(config_problems("build", self.rc.replace(
            "build:ci-linux-gpu --config=ci-platforms\n", "build:ci-linux-gpu --config=ci\n")))

    def test_gpu_platform_is_the_linux_ci_platform_asking_for_a_gpu(self):
        platforms = _platforms(_read(CI_BUILD))
        self.assertEqual([], platform_problems(platforms))
        gpu = platforms[GPU_PLATFORM]
        for mutation in (dict(gpu, parents=[":remote_macos_aarch64"]),
                         dict(gpu, exec_properties={"gpu": "present", "ISA": "x86_64"}),
                         dict(gpu, constraint_values=["@platforms//cpu:x86_64"])):
            with self.subTest(mutation=mutation):
                self.assertTrue(platform_problems(dict(platforms, **{GPU_PLATFORM: mutation})))
        self.assertTrue(platform_problems({BASE_PLATFORM: platforms[BASE_PLATFORM]}))

    def test_docs_describe_the_two_invocations(self):
        docs = _read(DOCS)
        self.assertIn("--config=" + CONFIG, docs)
        self.assertIn("--remote_instance_name", docs)


if __name__ == "__main__":
    unittest.main()
