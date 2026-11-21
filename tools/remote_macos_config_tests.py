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
GPU_PLATFORM = "//tools/ci:remote_macos_aarch64_gpu"
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


def _bzl_constant(name, path=XCODE_BZL):
    """A module-level literal assignment of remote_macos_xcode.bzl (Starlark parses as Python)."""
    for node in ast.parse(_read(path)).body:
        if isinstance(node, ast.Assign) and [target.id for target in node.targets] == [name]:
            return ast.literal_eval(node.value)
    raise AssertionError("%s does not define %s" % (path, name))


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


RULES_BZL = "build_defs/rules.bzl"
WRAPPER_IMPL = "_donner_transitioned_executable_impl"
# What the transitioned test wrapper may return. testing.ExecutionInfo would move its tests into
# their test exec group, on the base execution platform, away from the GPU variant's executors.
WRAPPER_PROVIDERS = {"DefaultInfo", "DonnerTransitionedBinaryInfo", "InstrumentedFilesInfo",
                     "RunEnvironmentInfo"}


def wrapper_provider_problems(rules):
    """Why the transitioned test wrapper's tests might not run on the target platform."""
    # Up to the next top-level definition; the body holds an unindented launcher script.
    match = re.search(r"(?ms)^def %s\(ctx\):\n(.*?)(?=^def |^\w+ = )" % WRAPPER_IMPL, rules)
    if not match:
        return ["%s is missing" % WRAPPER_IMPL]
    body = match.group(1)
    problems = ["%s returns testing.ExecutionInfo" % WRAPPER_IMPL] if "ExecutionInfo" in body \
        else []
    returned = set(re.findall(r"\b([A-Z][A-Za-z]*Info)\(", body)) | set(
        re.findall(r"\[([A-Z][A-Za-z]*Info)\]", body))
    problems += ["%s returns %s" % (WRAPPER_IMPL, name)
                 for name in sorted(returned - WRAPPER_PROVIDERS)]
    rules_using = re.findall(r"(?ms)^(\w+) = rule\(\n    implementation = %s," % WRAPPER_IMPL,
                             rules)
    if not {"_donner_multi_transitioned_test", "donner_transitioned_cc_test"} <= set(rules_using):
        problems.append("the transitioned test rules no longer use %s" % WRAPPER_IMPL)
    return problems


def output_directory_problems(rc):
    """A platform name in output directory names would split build keys between the variants."""
    return ["%r puts the platform in output directories" % line for line in rc.splitlines()
            if "platform_in_output_dir" in line.split("#", 1)[0]]


def _constraints(platforms, name):
    """A platform's effective constraint values, through its parents."""
    platform = platforms[name]
    inherited = set()
    for parent in platform.get("parents", []):
        inherited |= _constraints(platforms, parent.rsplit(":", 1)[1])
    return inherited | set(platform.get("constraint_values", []))


def _default_xcode():
    """The most preferred pinned Xcode with confirmed SDK versions: the static config's default."""
    confirmed = _bzl_constant("CONFIRMED_SDK_VERSIONS")
    return next(((version, build) for version, build in _pins() if build in confirmed), None)


class XcodePinAgreementTest(unittest.TestCase):
    def test_transition_path_matches_both_native_darwin_action_environments(self):
        rules = _read("build_defs/rules.bzl")
        rc = _read(".bazelrc")
        path = re.search(r'^_CI_DARWIN_PATH = "(PATH=[^"]+)"$', rules, re.MULTILINE).group(1)
        for flag in ("action_env", "host_action_env"):
            self.assertIn("build:ci-macos --%s=%s" % (flag, path), rc.splitlines())
        self.assertIn("/usr/bin", path.removeprefix("PATH=").split(":"))

    def test_darwin_transition_prioritizes_toolchains_used_by_test_launchers(self):
        transition = _read("build_defs/rules.bzl").split("def _ci_test_platform(", 1)[1].split("    return options", 1)[0]
        darwin = transition.split("    if attr.darwin:", 1)[1]
        self.assertIn('platforms = "//command_line_option:extra_execution_platforms"', darwin)
        self.assertIn('options[platforms] = [darwin] +', darwin)
        self.assertNotIn('options[platforms]', transition.split("    if attr.darwin:", 1)[0])

    def test_app_bundle_codesign_action_requires_a_mac_executor(self):
        bundle = _read("build_defs/macos_app_bundle.bzl")
        self.assertIn('exec_groups = {"codesign": exec_group(exec_compatible_with = ["@platforms//os:macos"])}',
                      bundle)
        action = bundle.split("ctx.actions.run_shell(", 1)[1].split("    )", 1)[0]
        self.assertIn('mnemonic = "DonnerMacosAppBundle"', action)
        self.assertIn('exec_group = "codesign"', action)

    def test_static_config_restates_the_pin_file_in_order(self):
        pins = _pins()
        self.assertTrue(pins, "%s names no Xcode" % PIN_FILE)
        self.assertEqual(pins, [tuple(pin) for pin in _bzl_constant("PINNED_XCODES", "build_defs/ci_xcode.bzl")])

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

    def test_static_xcode_targets_stay_out_of_wildcard_builds(self):
        # An xcode_config fails analysis when --xcode_version names a version it does not hold,
        # so a non-manual one breaks every wildcard build on another pinned Xcode.
        calls = {}
        for node in ast.walk(ast.parse(_read(XCODE_BZL))):
            if isinstance(node, ast.Call) and getattr(node.func, "id", None) in (
                    "xcode_config", "xcode_version"):
                calls.setdefault(node.func.id, []).append(
                    {keyword.arg: keyword.value for keyword in node.keywords})
        self.assertEqual({"xcode_config", "xcode_version"}, set(calls))
        for rule, instances in calls.items():
            for attributes in instances:
                with self.subTest(rule=rule):
                    self.assertIn("tags", attributes)
                    self.assertIn("manual", ast.literal_eval(attributes["tags"]))

    def test_build_defines_the_static_xcode_config(self):
        self.assertIn("remote_macos_xcodes", _calls(CI_BUILD, "remote_macos_xcode_config"))


class RemoteMacosConfigTest(unittest.TestCase):
    def test_config_targets_macos_on_one_remote_macos_execution_platform(self):
        for config, target in ((CONFIG, TARGET_PLATFORM), (GPU_CONFIG, GPU_PLATFORM)):
            for command in ("build", "test"):
                with self.subTest(config=config, command=command):
                    options = _expand(command, config)
                    self.assertEqual(target, bazelrc_configs.last_value(options, "platforms"))
                    # Execution platforms accumulate and the first compatible one wins.
                    self.assertEqual([EXEC_PLATFORM],
                                     _values(options, "extra_execution_platforms"))
                    # Bazel adds the host platform as the last execution-platform candidate.
                    self.assertEqual([EXEC_PLATFORM], _values(options, "host_platform"))

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

    def test_gpu_variant_builds_as_remote_macos_and_runs_only_tests_on_a_gpu(self):
        # Execution properties are part of an action's key: the variant keeps the execution
        # platform, so its build actions share remote-macos's cache entries, and runs each test
        # on the target platform, which asks for a GPU too.
        for command in ("build", "test"):
            with self.subTest(command=command):
                self.assertEqual(_expand(command, CONFIG) + [
                    "--platforms=" + GPU_PLATFORM, "--use_target_platform_for_tests"],
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
        gpu = platforms[GPU_PLATFORM.rsplit(":", 1)[1]]
        self.assertEqual([":" + EXEC_PLATFORM.rsplit(":", 1)[1]], gpu["parents"])
        self.assertEqual({"gpu": "present"}, gpu["exec_properties"])
        self.assertNotIn("constraint_values", gpu)

    def test_only_the_variants_use_the_base_and_no_workflow_names_them(self):
        # The base turns local fallback off (an exception to the BuildBuddy fallback policy), so it
        # must not reach any other config or any CI lane unseen. The one CI lane that selects the
        # variants does so through tools/ci/macos_remote_shadow.py, in an advisory job that
        # //tools:ci_macos_remote_shadow_tests pins.
        users = {name: [flag for _, flags in lines for flag in flags
                        if flag.startswith("--config=remote-macos")]
                 for name, lines in _configs().items()}
        self.assertEqual({CONFIG: ["--config=" + BASE_CONFIG], GPU_CONFIG: ["--config=" + CONFIG]},
                         {name: flags for name, flags in users.items() if flags})
        # Runfiles hold files, not directories, so find the tree from a file in it.
        main = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml"))
        github = main.parent.parent
        files = sorted((github / "workflows").glob("*.y*ml")) + sorted(
            (github / "actions").glob("*/action.y*ml"))
        # The lane table holds the Bazel commands of main.yml's steps.
        files.append(Path(runfiles.Create().Rlocation("donner/tools/ci/lanes.py")))
        self.assertTrue(files)
        for path in files:
            with self.subTest(path=path.name):
                self.assertNotIn("remote-macos", path.read_text(encoding="utf-8"))

    def test_gpu_tests_run_through_a_wrapper_that_returns_no_execution_info(self):
        rules = _read(RULES_BZL)
        self.assertEqual([], wrapper_provider_problems(rules))
        start = rules.index("def %s(ctx):" % WRAPPER_IMPL)
        end = rules.index("    return providers\n", start)
        for addition in ("    providers.append(testing.ExecutionInfo({}))\n",
                         "    providers.append(OutputGroupInfo())\n"):
            with self.subTest(addition=addition):
                self.assertTrue(wrapper_provider_problems(rules[:end] + addition + rules[end:]))

    def test_gpu_platform_has_the_target_platforms_constraints(self):
        # Equal constraints keep select() and target compatibility identical between variants.
        ci = _calls(CI_BUILD, "platform")
        root = _calls(ROOT_BUILD, "platform")
        self.assertEqual(_constraints(root, TARGET_PLATFORM.rsplit(":", 1)[1]),
                         _constraints(ci, GPU_PLATFORM.rsplit(":", 1)[1]))
        changed = dict(ci, remote_macos_aarch64_gpu=dict(
            ci["remote_macos_aarch64_gpu"], constraint_values=["@platforms//os:linux"]))
        self.assertNotEqual(_constraints(root, TARGET_PLATFORM.rsplit(":", 1)[1]),
                            _constraints(changed, GPU_PLATFORM.rsplit(":", 1)[1]))

    def test_output_directories_do_not_name_the_platform(self):
        rc = _read(".bazelrc")
        self.assertEqual([], output_directory_problems(rc))
        self.assertTrue(output_directory_problems(
            rc + "build --experimental_platform_in_output_dir\n"))

    def test_target_platform_is_macos_arm64(self):
        target = _calls(ROOT_BUILD, "platform")[TARGET_PLATFORM.rsplit(":", 1)[1]]
        self.assertEqual(["@platforms//cpu:aarch64", "@platforms//os:macos"],
                         sorted(target["constraint_values"]))


if __name__ == "__main__":
    unittest.main()
