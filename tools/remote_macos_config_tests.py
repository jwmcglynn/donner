"""Keeps `--config=remote-macos` complete, client-independent and on the approved Xcode.

The config builds and tests for macOS arm64 with every action on a remote macOS executor, from a
Linux or macOS client. It must name the macOS target platform, exactly one remote macOS execution
platform (also the host platform), the static Xcode configuration and remote strategies with no
local fallback, and it must
hold no private runner configuration: the executor endpoint, credentials and priority come from an
untracked rc file. A Linux and a macOS client must read the same flags from it, so it turns off
the host-OS configs that Bazel would otherwise choose by the client's OS.

The static Xcode configuration (//tools/ci:remote_macos_xcodes) restates tools/ci_macos_xcode.txt,
which Starlark cannot read without a repository rule, so these tests also fail when the two differ.
"""

import ast
from pathlib import Path
import re
import unittest

from python.runfiles import runfiles
from tools import bazelrc_configs


PIN_FILE = "tools/ci_macos_xcode.txt"
XCODE_BZL = "tools/ci/remote_macos_xcode.bzl"
CI_BUILD = "tools/ci/BUILD.bazel"
ROOT_BUILD = "BUILD.bazel"
DOCS = "docs/building.md"
BASE_CONFIG = "remote-macos-base"
CONFIG = "remote-macos"
GPU_CONFIG = "remote-macos-gpu"
TARGET_PLATFORM = "//:macos_aarch64"
EXEC_PLATFORM = "//tools/ci:remote_macos_aarch64"
GPU_EXEC_PLATFORM = "//tools/ci:remote_macos_aarch64_gpu"
XCODE_CONFIG = "//tools/ci:remote_macos_xcodes"
EXEC_PROPERTIES = {"OSFamily": "darwin", "ISA": "aarch64", "container-image": "none"}
SDK_PLATFORMS = ("ios", "macos", "tvos", "visionos", "watchos")
IDENTITY = "DONNER_CI_XCODE_IDENTITY"
# Private runner configuration: where the executor and cache are, how to authenticate to them,
# queue priority and per-host concurrency. None of it belongs in the repository's .bazelrc. The
# number of GPU test slots (`--local_resources=local_gpu=`) is public scheduling policy for local
# GPU tests, which the macOS CI config may set, and has no effect under remote execution; CPU and
# memory budgets describe a particular host and stay private.
PRIVATE_FLAG = re.compile(
    r"^--(?:no)?(?:remote_executor|remote_cache|remote_instance_name|remote_header|"
    r"remote_cache_header|remote_exec_header|remote_downloader_header|remote_execution_priority|"
    r"remote_proxy|tls_|credential_helper|google_|bes_|jobs|local_resources(?!=local_gpu=))")
# Bazel's names for the client OS in host-OS config lines (`build:linux`, `build:macos`).
CLIENT_OSES = ("linux", "macos")


def _read(path):
    return Path(runfiles.Create().Rlocation("donner/%s" % path)).read_text(encoding="utf-8")


def _pins():
    """(version, build) pairs of the pin file, in its preference order."""
    return [tuple(line.split()) for line in _read(PIN_FILE).splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


def _bzl_constant(name):
    """A module-level literal assignment of remote_macos_xcode.bzl (Starlark parses as Python)."""
    for node in ast.parse(_read(XCODE_BZL)).body:
        if isinstance(node, ast.Assign) and [target.id for target in node.targets] == [name]:
            return ast.literal_eval(node.value)
    raise AssertionError("%s does not define %s" % (XCODE_BZL, name))


def _calls(path, function):
    """{name: {attribute: literal}} for each top-level `function(...)` call in a BUILD file."""
    calls = {}
    for node in ast.parse(_read(path)).body:
        if (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
                and getattr(node.value.func, "id", None) == function):
            attributes = {keyword.arg: ast.literal_eval(keyword.value)
                          for keyword in node.value.keywords}
            calls[attributes["name"]] = attributes
    return calls


def _configs():
    return bazelrc_configs.parse(_read(".bazelrc"))


def _expand(command, config):
    return bazelrc_configs.expand(command, ["--config=" + config], _configs())


def _values(options, name):
    """Every value of `--<name>=`, in order."""
    return [option.split("=", 1)[1] for option in options if option.startswith("--%s=" % name)]


def _last_boolean(options, name, default):
    value = default
    for option in options:
        if option in ("--" + name, "--%s=true" % name, "--%s=1" % name):
            value = True
        elif option in ("--no" + name, "--%s=false" % name, "--%s=0" % name):
            value = False
    return value


def _client_options(command, config, client_os):
    """The repository flags a client on `client_os` reads for `bazel <command> --config=<config>`.

    This models only Bazel's decision whether to apply the host-OS config (`<command>:<os>` lines):
    it takes that decision from the value of --enable_platform_specific_config once every --config
    on the command line has been expanded. It does not model where the host-OS flags land.
    """
    global_options = []
    for line in _read(".bazelrc").splitlines():
        words = line.split()
        if words and words[0] in bazelrc_configs.sections(command):
            global_options.extend(words[1:])
    options = _expand(command, config)
    if _last_boolean(global_options + options, "enable_platform_specific_config", False):
        options = options + bazelrc_configs.expand(command, ["--config=" + client_os], _configs())
    return options


def _default_xcode():
    """The most preferred pinned Xcode with confirmed SDK versions: the static config's default."""
    confirmed = _bzl_constant("CONFIRMED_SDK_VERSIONS")
    return next(((version, build) for version, build in _pins() if build in confirmed), None)


class XcodePinAgreementTest(unittest.TestCase):
    def test_static_config_restates_the_pin_file_in_order(self):
        pins = _pins()
        self.assertTrue(pins, "%s names no Xcode" % PIN_FILE)
        self.assertEqual(pins, [tuple(pin) for pin in _bzl_constant("PINNED_XCODES")])

    def test_confirmed_sdk_versions_belong_to_pinned_builds(self):
        pinned_builds = {build for _, build in _pins()}
        confirmed = _bzl_constant("CONFIRMED_SDK_VERSIONS")
        self.assertTrue(confirmed, "no pinned Xcode has confirmed SDK versions")
        for build, sdks in confirmed.items():
            with self.subTest(build=build):
                self.assertIn(build, pinned_builds, "stale SDK entry for an unpinned build")
                self.assertEqual(set(SDK_PLATFORMS), set(sdks))
                for sdk in sdks.values():
                    self.assertRegex(sdk, r"^[0-9]+\.[0-9]+(\.[0-9]+)?$")

    def test_config_selects_the_static_default_xcode(self):
        default = _default_xcode()
        self.assertIsNotNone(default, "no pinned Xcode has confirmed SDK versions")
        version, _ = default
        for config in (CONFIG, GPU_CONFIG):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    options = _expand(command, config)
                    self.assertEqual([XCODE_CONFIG], _values(options, "xcode_version_config"))
                    self.assertEqual([version], _values(options, "xcode_version"))

    def test_toolchain_identity_is_left_to_the_caller(self):
        # As on the CI lanes, only whoever knows the executor's actual Xcode may name it; the
        # repository's global lines pass the caller's value through to action and test keys.
        for config in (CONFIG, GPU_CONFIG):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    self.assertEqual([], [option for option in _expand(command, config)
                                          if IDENTITY in option])

    def test_docs_name_the_identity_of_the_default_xcode(self):
        version, build = _default_xcode()
        named = set(re.findall(r"%s=([0-9.]+-[0-9A-Za-z]+)" % IDENTITY, _read(DOCS)))
        self.assertEqual({"%s-%s" % (version, build)}, named)

    def test_build_defines_the_static_xcode_config(self):
        self.assertIn("remote_macos_xcodes", _calls(CI_BUILD, "remote_macos_xcode_config"))


class RemoteMacosConfigTest(unittest.TestCase):
    def test_config_targets_macos_on_one_remote_macos_execution_platform(self):
        for config, platform in ((CONFIG, EXEC_PLATFORM), (GPU_CONFIG, GPU_EXEC_PLATFORM)):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    options = _expand(command, config)
                    self.assertEqual(TARGET_PLATFORM, bazelrc_configs.last_value(options,
                                                                                 "platforms"))
                    # Execution platforms accumulate and the first compatible one wins.
                    self.assertEqual([platform], _values(options, "extra_execution_platforms"))
                    # Bazel adds the host platform as the last execution-platform candidate.
                    self.assertEqual([platform], _values(options, "host_platform"))

    def test_every_action_runs_remotely_without_local_fallback(self):
        for config in (CONFIG, GPU_CONFIG):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    options = _expand(command, config)
                    self.assertEqual("remote", bazelrc_configs.last_value(options,
                                                                          "spawn_strategy"))
                    strategies = dict(value.split("=", 1) for value in _values(options, "strategy"))
                    for mnemonic in ("CppCompile", "CppLink", "TestRunner"):
                        self.assertEqual("remote", strategies.get(mnemonic), mnemonic)
                    self.assertEqual([], _values(options, "strategy_regexp"))
                    self.assertFalse(_last_boolean(options, "remote_local_fallback", True))

    def test_config_composes_the_macos_ci_and_generic_remote_configs(self):
        options = _expand("test", CONFIG)
        for nested in ("macos", "ci-macos", "re"):
            with self.subTest(config=nested):
                nested_options = _expand("test", nested)
                self.assertTrue(nested_options)
                for option in nested_options:
                    self.assertIn(option, options)

    def test_gpu_variant_differs_only_in_its_execution_platform(self):
        for command in ("build", "test"):
            with self.subTest(command=command):
                self.assertEqual([option.replace("=" + EXEC_PLATFORM, "=" + GPU_EXEC_PLATFORM)
                                  for option in _expand(command, CONFIG)],
                                 _expand(command, GPU_CONFIG))

    def test_repository_config_holds_no_private_runner_configuration(self):
        for config in (CONFIG, GPU_CONFIG):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    private = [option for option in _expand(command, config)
                               if PRIVATE_FLAG.match(option)]
                    self.assertEqual([], private)

    def test_linux_and_macos_clients_read_the_same_flags(self):
        rc = _read(".bazelrc")
        # Without this global line the host-OS configs never apply and the check proves nothing.
        self.assertIn("\nbuild --enable_platform_specific_config\n", rc)
        self.assertIn("build:linux ", rc)
        for config in (CONFIG, GPU_CONFIG):
            for command in ("build", "test", "coverage"):
                with self.subTest(config=config, command=command):
                    linux, macos = (_client_options(command, config, client_os)
                                    for client_os in CLIENT_OSES)
                    self.assertEqual(linux, macos)
                    self.assertNotIn("--copt=-fno-modules", linux)

    def test_execution_platforms_carry_the_generic_macos_properties(self):
        platforms = _calls(CI_BUILD, "platform")
        base = platforms[EXEC_PLATFORM.rsplit(":", 1)[1]]
        self.assertEqual(["@platforms//cpu:aarch64", "@platforms//os:macos"],
                         sorted(base["constraint_values"]))
        self.assertEqual(EXEC_PROPERTIES, base["exec_properties"])
        self.assertNotIn("parents", base)
        gpu = platforms[GPU_EXEC_PLATFORM.rsplit(":", 1)[1]]
        self.assertEqual([":" + EXEC_PLATFORM.rsplit(":", 1)[1]], gpu["parents"])
        self.assertEqual({"gpu": "present"}, gpu["exec_properties"])
        self.assertNotIn("constraint_values", gpu)

    def test_only_the_two_variants_use_the_base_and_no_workflow_selects_them(self):
        # The base turns local fallback off (an exception to the BuildBuddy fallback policy), so it
        # must not reach any other config or any CI lane unseen.
        users = {name for name, lines in _configs().items()
                 for _, flags in lines if "--config=" + BASE_CONFIG in flags}
        self.assertEqual({CONFIG, GPU_CONFIG}, users)
        # Runfiles hold files, not directories, so find the tree from a file in it.
        main = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml"))
        github = main.parent.parent
        files = sorted((github / "workflows").glob("*.y*ml")) + sorted(
            (github / "actions").glob("*/action.y*ml"))
        self.assertTrue(files)
        for path in files:
            with self.subTest(path=path.name):
                self.assertNotIn("remote-macos", path.read_text(encoding="utf-8"))

    def test_target_platform_is_macos_arm64(self):
        target = _calls(ROOT_BUILD, "platform")[TARGET_PLATFORM.rsplit(":", 1)[1]]
        self.assertEqual(["@platforms//cpu:aarch64", "@platforms//os:macos"],
                         sorted(target["constraint_values"]))


if __name__ == "__main__":
    unittest.main()
