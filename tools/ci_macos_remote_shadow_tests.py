"""Pins the advisory macOS remote-execution shadow lane (tools/ci/macos_remote_shadow.py).

The lane builds and tests a trusted pull request's selection for macOS from a self-hosted Linux
runner, every action on a remote macOS executor. These tests keep it reachable only through the
self-hosted Linux lane's trust gate plus an opt-in, keep it from ever failing a pull request or
the coverage contract, keep private runner configuration out of the repository, and keep what it
runs a documented subset of the self-hosted macOS lane.

Each workflow rule is a function that returns the problems it finds, so the tests show it passes
on the real workflow and fails on a workflow that breaks it.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

from python.runfiles import runfiles
from tools import bazelrc_configs
from tools.ci import lanes
from tools.ci import macos_remote_shadow as lane
from tools.test_executable_fixtures import ExecutableFixtureDirectory

MAIN = ".github/workflows/main.yml"
JOB = "macos-remote-shadow"
TURNSTILE = JOB + "-turnstile"
LINUX_JOB = "linux-self-hosted"
LINUX_TURNSTILE = LINUX_JOB + "-turnstile"
MACOS_JOB = "macos-self-hosted"
LABEL = "ci:macos-remote-shadow"
VARIABLE = "MACOS_REMOTE_SHADOW_LANE"
# What the shadow lane adds to the self-hosted Linux lane's trust gate. It only narrows the gate.
OPT_IN = (" && github.event_name == 'pull_request' && (contains("
          "github.event.pull_request.labels.*.name, '%s') || vars.%s == 'true')"
          % (LABEL, VARIABLE))
# Before the trust gate: the lane runs after its run's Linux lane, whether that passed or failed.
AFTER_LINUX = ("!cancelled() && (needs.linux-self-hosted.result == 'success' || "
               "needs.linux-self-hosted.result == 'failure') && ")
# After the opt-in, on the lane job only: its own admission let it run.
ADMITTED = " && needs.%s.outputs.admitted == 'true'" % (JOB + "-turnstile")
# The lane step runs the lane table's one phase for the lane, as every lane step does.
RUN = "python3 tools/ci/lane.py phase macos-remote-shadow shadow"
SELECTION_ENV = {"TARGETS_FALLBACK": "${{ needs.determine-targets.outputs.fallback }}",
                 "TARGETS_AFFECTED": "${{ needs.determine-targets.outputs.affected }}"}


def _shadow_phase():
    """The lane table's phase for the shadow lane."""
    return lanes.LANES[JOB].phases["shadow"]


def _script_argv(phase=None):
    """The script command of the shadow phase, as words, the selection as SELECTED."""
    phase = phase or _shadow_phase()
    commands = [action for action in phase.actions if isinstance(action, lanes.Command)]
    return list(commands[0].argv) if len(commands) == 1 else []
# Private runner configuration that must come only from the runner's own rc files: endpoints,
# credentials, priority and per-host concurrency (as in //tools:remote_macos_config_tests).
PRIVATE_FLAG = re.compile(
    r"--(?:no)?(?:remote_executor|remote_cache|remote_instance_name|remote_header|"
    r"remote_cache_header|remote_exec_header|remote_downloader_header|remote_execution_priority|"
    r"remote_proxy|tls_|credential_helper|google_|bes_|local_resources)")
ENDPOINT = re.compile(r"\b(?:grpcs?|https?|unix)://")
# The reviewed test-artifact collector every test lane uploads its failed tests' outputs with.
COLLECTOR = "./.github/actions/upload-bazel-test-artifacts"
# The lane step continues on error, so failure() is never true after it; its outcome is.
FAILURE_UPLOAD_IF = "${{ !cancelled() && steps.shadow.outcome == 'failure' }}"
# Flags of `--config=ci` the lane leaves out, with why.
CI_OMISSIONS = {
    "--repository_cache=~/.cache/bazel-repo": "A runner path; the runner's rc can set its own.",
}


# What the lane script, its Bazel wrappers, the test-artifact collector and the stand-in Bazel
# read from their environment: the planner's FALLBACK and AFFECTED, DONNER_CI_XCODE_IDENTITY and
# the other DONNER_CI_* values, GITHUB_STEP_SUMMARY and the other GITHUB_* values, RUNNER_TEMP,
# the watchdog's BAZEL_STALL_LIMIT_SECONDS and BAZEL_HEARTBEAT_INTERVAL_SECONDS, the collector's
# TESTLOGS and every FAKE_* switch. Every child environment of these tests starts from a copy of
# the test's own environment without them, so a CI lane's values (the macOS lanes export
# DONNER_CI_XCODE_IDENTITY) cannot change what a test sees.
_SCRUBBED_PREFIXES = ("DONNER_", "GITHUB_", "RUNNER_", "BAZEL_", "FAKE_")
_SCRUBBED_NAMES = ("FALLBACK", "AFFECTED", "TESTLOGS", "USE_BAZEL_VERSION")


def _environ(**values):
    """A child environment: this test's environment without the lane's inputs, plus `values`."""
    environ = {name: value for name, value in os.environ.items()
               if not name.startswith(_SCRUBBED_PREFIXES) and name not in _SCRUBBED_NAMES}
    environ.update(values)
    return environ


def _signallable(pid):
    """Whether a process is still there for this test to signal, by the lane's own rule for a
    process group (_signal_group): EPERM, for a process this test started as its own user, means
    one that has exited and waits to be reaped, or one out of its reach, which a signal could not
    end either."""
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return False
    return True


class _GatedClock:
    """Stands still until `ready()` holds, then jumps far past any deadline set from it: a test's
    deadline is reached only once the state it observes exists, however slowly the host starts
    processes."""

    def __init__(self, ready):
        self.ready = ready
        self.start = time.monotonic()
        self.jumped = False

    def __call__(self):
        if not self.jumped and self.ready():
            self.jumped = True
        return self.start + (10 ** 9 if self.jumped else 0)


def _read(path):
    return Path(runfiles.Create().Rlocation("donner/%s" % path)).read_text(encoding="utf-8")


def _root():
    return Path(runfiles.Create().Rlocation("donner/.bazelrc")).parent


def _preflights(out):
    return [command for command, _ in lane.preflight_commands("bazelisk", [], [], out).values()]


def _routing(out):
    """The routing probe's Bazel command, from the Bazel executable on."""
    return _bazel_part(lane.routing_command("bazelisk", [], [], out)[0])


def _bazel_part(command):
    """A phase command from the Bazel executable on."""
    return command[command.index("bazelisk"):]


def _tag_filter_sets(value):
    tags = value.split(",")
    return ({tag for tag in tags if not tag.startswith("-")},
            {tag[1:] for tag in tags if tag.startswith("-")})


class WorkflowTest(unittest.TestCase):
    def test_standalone_diagnostic_has_no_github_runner_or_admission_pool(self):
        workflow = _read(".github/workflows/main.yml")
        self.assertNotIn("\n  macos-remote-shadow:", workflow)
        self.assertNotIn("\n  macos-remote-shadow-turnstile:", workflow)
        self.assertNotIn("MACOS_REMOTE_SHADOW_LANE", workflow)
        self.assertIn("tools/ci/macos_remote_shadow.py", _read("docs/building.md"))

    def test_mixed_workflow_and_live_artifact_collector_keep_private_settings_out(self):
        workflow = _read(".github/workflows/main.yml")
        step = workflow.split("      - name: Test Linux and macOS with native platform transitions", 1)[1]
        step = step.split("      - name:", 1)[0]
        sources = {"mixed invocation": step, "artifact collector": _read("tools/ci/macos_remote_shadow.py"),
                   "receipt verifier": _read("tools/ci/verify_unified_tests.py")}
        def exposed(text):
            return bool(PRIVATE_FLAG.search(text) or ENDPOINT.search(text))
        for owner, text in sources.items():
            with self.subTest(source=owner):
                self.assertFalse(exposed(text))
                for leak in ("--remote_executor=grpcs://executor.invalid:1", "--tls_client_key=k.pem",
                             "--remote_execution_priority=0", "grpcs://cache.invalid"):
                    self.assertTrue(exposed(text + "\n" + leak))

    def test_mixed_failure_archive_accepts_only_staged_test_outputs(self):
        workflow = _read(".github/workflows/main.yml")
        owner = workflow.split("      - name: Upload mixed-platform test failure artifacts", 1)[1]
        owner = owner.split("      - name:", 1)[0]
        self.assertIn("if: failure() && steps.unified.outcome == 'failure'", owner)
        self.assertIn("uses: " + COLLECTOR, owner)
        def approved(block):
            return re.findall(r"(?m)^ +testlogs: (.+)$", block) == [
                "${{ runner.temp }}/unified/test-artifacts/mixed"]
        self.assertTrue(approved(owner))
        for unsafe in ("", "/test/bep.json", "/test/exec.log.zst", "/.bazelrc"):
            self.assertFalse(approved(owner.replace("/unified/test-artifacts/mixed", unsafe)))

    def test_watchdog_is_the_linux_lane_watchdog(self):
        # The shadow lane runs its Bazel commands under the self-hosted Linux lane's own stall
        # watchdog, which //tools:ci_runtime_workflow_tests exercises.
        self.assertEqual("tools/ci/bazel_watchdog.sh", lane.WATCHDOG)
        self.assertEqual(lane.WATCHDOG, lanes.watchdog("test")[0])
        self.assertIn(lanes.watchdog("test"), [
            tuple(action.wrappers[1:3]) for action in
            lanes.LANES[LINUX_JOB].phases["test"].actions if isinstance(action, lanes.Bazel)])


class LaneCommandTest(unittest.TestCase):
    maxDiff = None
    OUT = "/runner/temp/macos-remote-shadow"
    TARGETS = ["//donner/base:base_tests", "//donner/svg/renderer/geode:geode_device_tests"]

    @classmethod
    def setUpClass(cls):
        cls.configs = bazelrc_configs.parse(_read(".bazelrc"))
        cls.commands = {phase: lane.phase_command(phase, "bazelisk", [], [], cls.OUT, cls.TARGETS)
                        for phase in lane.PHASES}

    def _expanded(self, phase):
        command = _bazel_part(self.commands[phase])
        return bazelrc_configs.expand("test", command[2:], self.configs)

    def test_phases_select_their_configs_and_cap_the_gpu_phase(self):
        native, gpu = (_bazel_part(self.commands[phase]) for phase in lane.PHASES)
        self.assertEqual(["bazelisk", "test", "--config=remote-macos"], native[:3])
        self.assertEqual(["bazelisk", "test", "--config=remote-macos-gpu"], gpu[:3])
        for command, config in ((native, "--config=remote-macos"),
                                (gpu, "--config=remote-macos-gpu")):
            self.assertEqual([config, "--config=ci-test-lane"],
                             [word for word in command if word.startswith("--config")])
        self.assertNotIn("--build_tests_only", native)
        self.assertIn("--build_tests_only", gpu)
        # The cap is the last --jobs of the expanded command, so neither the config nor anything
        # bound to it in a runner's rc can raise it; the native phase sets none.
        self.assertEqual("4", bazelrc_configs.last_value(self._expanded("gpu"), "jobs"))
        self.assertIsNone(bazelrc_configs.last_value(self._expanded("native"), "jobs"))
        rc_options = ["--jobs=64"]
        capped = lane.phase_command("gpu", "bazelisk", [], rc_options, self.OUT, self.TARGETS)
        self.assertLess(capped.index("--jobs=64"), capped.index("--jobs=4"))
        self.assertEqual(lane.GPU_JOBS, 4)
        self.assertEqual(self.TARGETS, native[-2:])
        self.assertEqual(self.TARGETS, gpu[-2:])

    def test_both_phases_keep_going_past_a_failed_action(self):
        # An advisory lane: one failed action must not hide everything that does not depend on it.
        for phase in lane.PHASES:
            self.assertIn("--keep_going", _bazel_part(self.commands[phase]), phase)
            # Nothing the config expands to turns it off again.
            settings = [word for word in self._expanded(phase)
                        if re.fullmatch(r"--(?:no)?keep_going(?:=.*)?|-k", word)]
            self.assertEqual("--keep_going", settings[-1], phase)
        # Only the phases: a failed preflight probe stops the lane at once anyway.
        for command in [_routing(self.OUT)] + list(_preflights(self.OUT)):
            self.assertNotIn("--keep_going", command)

    def test_phases_run_under_the_empty_selection_wrapper_and_the_watchdog(self):
        for phase in lane.PHASES:
            with self.subTest(phase=phase):
                command = self.commands[phase]
                self.assertEqual([lane.TEST_WRAPPER, lane.WATCHDOG,
                                  "%s/%s/console.log" % (self.OUT, phase), "bazelisk"],
                                 command[:4])

    def test_lane_never_passes_the_ci_config_but_keeps_what_it_needs(self):
        ci = bazelrc_configs.expand("test", ["--config=ci"], self.configs)
        self.assertTrue(ci)
        lane_probe, plain_probe = _preflights(self.OUT)
        self.assertIn("--config=remote-macos", lane_probe)
        self.assertEqual([], [word for word in plain_probe if word.startswith("--config")])
        for name, command in [("preflight", lane_probe), ("preflight", plain_probe),
                              ("preflight", _routing(self.OUT))] + [
                (phase, _bazel_part(self.commands[phase])) for phase in lane.PHASES]:
            with self.subTest(command=name):
                words = " ".join(command)
                self.assertNotRegex(words, r"--config[= ]ci(?:\s|$)")
                expanded = bazelrc_configs.expand(command[1], command[2:], self.configs)
                if name == "preflight":
                    # It analyzes nothing, so it needs only the build event and target flags.
                    self.assertEqual([], [flag for flag in lane.CI_FLAGS if flag not in expanded])
                    continue
                # The lane states its own tag filters.
                missing = [flag for flag in ci if flag not in expanded and
                           not flag.startswith("--test_tag_filters=")]
                self.assertEqual(sorted(CI_OMISSIONS), sorted(missing))

    def test_lane_leaves_every_runner_setting_to_the_runners_rc(self):
        # The runner's rc binds these to remote-macos-base; a command-line value would override
        # it, and the `ci`, `re` and other lane configs carry another lane's executor settings.
        forbidden = re.compile(
            r"^--(?:config=(?:ci|re|buildbuddy-re|macos-vm|hosted-macos[a-z-]*)$|remote_executor|"
            r"remote_cache|tls_|remote_default_exec_properties|experimental_remote_downloader|"
            r"remote_execution_priority|remote_timeout|(?:no)?enable_platform_specific_config|"
            r"strategy|spawn_strategy|(?:no)?remote_local_fallback|"
            r"incompatible_remote_local_fallback|announce_rc)")
        lane_probe, plain_probe = _preflights(self.OUT)
        commands = {"preflight": lane_probe, "plain preflight": plain_probe,
                    "routing probe": _routing(self.OUT)}
        commands.update((phase, _bazel_part(self.commands[phase])) for phase in lane.PHASES)
        for name, command in commands.items():
            with self.subTest(command=name):
                self.assertEqual([], [flag for flag in command if forbidden.match(flag)])
                self.assertIn(command[1], ("build", "test"))
        self.assertEqual([], [flag for flag in commands["native"] if flag.startswith("--jobs")])
        self.assertNotRegex(_read("tools/ci/macos_remote_shadow.py"),
                            r"[\"'](?:query|coverage|info)[\"']")

    def test_repository_rules_keep_the_client_environment_on_every_command(self):
        rc_options = ["--jobs=64"]
        lane_probe, plain_probe = (command for command, _ in lane.preflight_commands(
            "bazelisk", [], rc_options, self.OUT).values())
        commands = {"preflight": lane_probe, "plain preflight": plain_probe,
                    "routing probe": _bazel_part(lane.routing_command(
                        "bazelisk", [], rc_options, self.OUT)[0])}
        commands.update((phase, _bazel_part(lane.phase_command(
            phase, "bazelisk", [], rc_options, self.OUT, self.TARGETS))) for phase in lane.PHASES)
        self.assertEqual("--incompatible_repo_env_ignores_action_env", lane.CLIENT_REPO_ENV)
        for name, command in commands.items():
            with self.subTest(command=name):
                self.assertEqual(1, command.count(lane.CLIENT_REPO_ENV))
                position = command.index(lane.CLIENT_REPO_ENV)
                # After the remote macOS config and the rc options, so neither can turn it off.
                before = [word for word in command[position:]
                          if word.startswith("--config=remote-macos") or word in rc_options]
                self.assertEqual([], before)
        # The mixed-platform canary also runs repository rules on the client.
        rc_lines = [line.partition("#")[0].split() for line in _read(".bazelrc").splitlines()]
        settings = [words for words in rc_lines
                    if any("repo_env_ignores_action_env" in word for word in words)]
        self.assertEqual([["build:ci-platforms", lane.CLIENT_REPO_ENV]], settings)
        self.assertIn(["build:ci-platforms", "--config=latest_llvm"], rc_lines)

    def test_remote_execution_messages_of_failures_are_printed(self):
        # Bazel's default prints the messages a backend attaches to a failed remote action, which
        # are often what explains it; no lane command quiets them.
        commands = dict((phase, self.commands[phase]) for phase in lane.PHASES)
        commands.update(lane_probe=_preflights(self.OUT)[0], plain_probe=_preflights(self.OUT)[1],
                        routing=_routing(self.OUT))
        for name, command in commands.items():
            values = [flag for flag in command
                      if flag.startswith("--remote_print_execution_messages")]
            self.assertEqual([], values, name)

    def test_every_bazel_command_uses_the_lanes_own_output_root_and_idle_limit(self):
        environ = {"FALLBACK": "true"}
        lines = _capture_main(["--dry-run", "--out", self.OUT, "--output-user-root", "/root/x"],
                              environ)[1:]
        self.assertEqual(6, len(lines))  # Two probes, the routing probe, two phases, shutdown.
        for line in lines:
            words = shlex.split(line)
            with self.subTest(line=line[:60]):
                bazel = words.index("bazelisk")
                self.assertEqual(["--output_user_root=/root/x", "--max_idle_secs=600"],
                                 words[bazel + 1:bazel + 3])
        self.assertTrue(lines[-1].endswith(
            "bazelisk --output_user_root=/root/x --max_idle_secs=600 shutdown"))
        # By default it lives outside the workspace, in the runner's persistent tool cache.
        args = lane.parse_args([], {})
        self.assertTrue(lane._default_locations(args, {"RUNNER_TEMP": "/t",
                                                       "RUNNER_TOOL_CACHE": "/c"}))
        self.assertEqual(("/t/macos-remote-shadow", "/c/donner-macos-remote-shadow-output"),
                         (args.out, args.output_user_root))
        self.assertFalse(lane._default_locations(lane.parse_args([], {}), {}))

    def test_routing_probe_is_a_manual_uncached_test(self):
        probe = re.search(r'(?ms)^sh_test\(\n    name = "remote_macos_probe",\n(.*?)^\)$',
                          _read("tools/ci/BUILD.bazel"))
        self.assertIsNotNone(probe)
        tags = re.findall(r'"([^"]+)"', probe.group(1).split("tags = [", 1)[1].split("]", 1)[0])
        self.assertEqual(["external", "manual"], sorted(tags))
        routing = _routing(self.OUT)
        self.assertEqual("test", routing[1])
        self.assertIn("--nocache_test_results", routing)
        self.assertEqual(lane.ROUTING_PROBE, routing[-1])
        # The options that differed from the native phase made Bazel discard this analysis.
        native = _bazel_part(self.commands["native"])
        for option in (lane.TEST_LANE_CONFIG, lane.LANE_TEST_ENV):
            self.assertIn(option, routing)
            self.assertIn(option, native)

    def test_editor_build_identity_comes_from_the_test_lane_config(self):
        # The narrow config `ci` composes for every non-release lane; never `ci` itself.
        for phase in lane.PHASES:
            with self.subTest(phase=phase):
                self.assertIn("--config=ci-test-lane", self.commands[phase])
                expanded = self._expanded(phase)
                self.assertEqual(["0" * 40], [flag.split("=", 1)[1] for flag in expanded
                                              if flag.startswith(
                                                  "--//build_defs:release_source_commit=")])
        rule = _read("build_defs/editor_build_info.bzl")
        self.assertIn('execution_requirements = {} if commit else {"local": "1"}', rule)

    def test_tag_filters_split_gpu_tests_from_everything_else(self):
        native_positive, native_negative = _tag_filter_sets(lane.tag_filters("native"))
        gpu_positive, gpu_negative = _tag_filter_sets(lane.tag_filters("gpu"))
        self.assertEqual(set(), native_positive)
        self.assertEqual(set(lane.GPU_TAGS), gpu_positive)
        self.assertEqual(set(lane.EXCLUDED_TAGS) | set(lane.GPU_TAGS), native_negative)
        self.assertEqual(set(lane.EXCLUDED_TAGS), gpu_negative)
        for phase in lane.PHASES:
            values = [flag.split("=", 1)[1] for flag in self.commands[phase]
                      if flag.startswith("--test_tag_filters=")]
            self.assertEqual([lane.tag_filters(phase)], values)
        for tag in ("launches-browser", "requires_desktop_session", "local", "no-remote",
                    "manual", "perf", "fuzz_soak"):
            self.assertIn(tag, lane.EXCLUDED_TAGS)

    def test_lane_runs_a_subset_of_the_self_hosted_macos_lane(self):
        test = next(action for action in lanes.LANES[MACOS_JOB].phases["test"].actions
                    if isinstance(action, lanes.Bazel))
        expanded = bazelrc_configs.expand("test", [word for word in test.args
                                                   if isinstance(word, str)], self.configs)
        filters = bazelrc_configs.last_value(expanded, "test_tag_filters")
        positive, negative = _tag_filter_sets(filters)
        self.assertEqual(set(), positive)
        self.assertEqual(set(), negative - set(lane.EXCLUDED_TAGS))

    def test_gpu_tags_are_the_tags_that_mark_gpu_opening_tests(self):
        rules = _read("build_defs/rules.bzl")
        constants = dict(re.findall(r'(?m)^([A-Z_]+) = "([^"]+)"$', rules))
        block = rules.split("and opens_gpu_device:\n", 1)[1].split("\n    _donner_multi", 1)[0]
        added = set()
        for condition, value in re.findall(
                r"(?m)^        if (.*):\n            tags = tags \+ \[([^\]]+)\]$", block):
            if "remote_parallel_ci" not in condition:
                added.add(constants.get(value, value.strip('"')))
        self.assertTrue(added)
        self.assertLessEqual(added, set(lane.GPU_TAGS))

    def test_xcode_identity_is_the_default_pinned_xcode_in_the_docs(self):
        identity = lane.xcode_identity(_root())
        self.assertEqual({identity}, set(re.findall(
            r"%s=([0-9.]+-[0-9A-Za-z]+)" % lane.IDENTITY, _read("docs/building.md"))))
        options = bazelrc_configs.expand("build", ["--config=remote-macos"], self.configs)
        self.assertEqual(identity.split("-")[0], bazelrc_configs.last_value(options,
                                                                            "xcode_version"))

    def test_selection_and_dry_run(self):
        self.assertEqual(["//..."], lane.selection("true", "//a:b"))
        self.assertEqual(["//a:b", "//c:d"], lane.selection("false", " //a:b  //c:d "))
        self.assertEqual([], lane.selection("false", "  "))
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--dry-run",
                 "--out", directory, "--fallback", "false", "--affected", " ".join(self.TARGETS)],
                capture_output=True, text=True, check=False,
                env=_environ(DONNER_CI_XCODE_IDENTITY=""))
        self.assertNotEqual(0, result.returncode, "an empty identity contradicts the config")
        environ = {"FALLBACK": "false", "AFFECTED": " ".join(self.TARGETS)}
        lines = _capture_main(["--dry-run", "--out", self.OUT], environ)
        self.assertEqual("%s=%s" % (lane.IDENTITY, lane.xcode_identity(_root())), lines[0])
        startup = [lane.MAX_IDLE]
        preflights = [command for command, _ in lane.preflight_commands(
            "bazelisk", startup, [], self.OUT).values()]
        routing = lane.routing_command("bazelisk", startup, [], self.OUT)[0]
        phases = [lane.phase_command(phase, "bazelisk", startup, [], self.OUT, self.TARGETS)
                  for phase in lane.PHASES]
        self.assertEqual([shlex.join(command) for command in preflights + [routing] + phases] +
                         ["bazelisk --max_idle_secs=600 shutdown"], lines[1:])


def _capture_main(argv, environ):
    import contextlib
    import io
    stream = io.StringIO()
    with contextlib.redirect_stdout(stream):
        status = lane.main(argv, environ)
    if status:
        raise AssertionError("main exited %d" % status)
    return stream.getvalue().splitlines()


def _events(*events):
    return [json.dumps(event) for event in events]


def _canonical(options):
    return {"id": {"structuredCommandLine": {"commandLineLabel": "canonical"}},
            "structuredCommandLine": {"sections": [{"sectionLabel": "command options",
                                                    "optionList": {"option": options}}]}}


_FAKE_BAZEL = r'''
import json, os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(args) + "\n")
if "shutdown" in args:
    sys.exit(0)
if any(a.startswith("--config=remote-macos") for a in args) and (
        os.environ.get("FAKE_REPOSITORY_FAILURE") or (
            os.environ.get("FAKE_RC_EMPTIES_ACTION_ENV") and
            "--incompatible_repo_env_ignores_action_env" not in args)):
    # A runner rc that empties a library path with --action_env under the remote macOS config:
    # without the flag Bazel 8 hands that to repository rules, which run on this client.
    print("process-wrapper: error while loading shared libraries: libstdc++.so.6: cannot open "
          "shared object file: No such file or directory")
    print("ERROR: Error computing the main repository mapping: error running 'git init'")
    sys.exit(37)
events = next(a.split("=", 1)[1] for a in args if a.startswith("--build_event_json_file="))
os.makedirs(os.path.dirname(events), exist_ok=True)
lines = []
if "//tools/ci:remote_macos_probe" in args:
    # The routing probe test passes only on macOS arm64; its result says how it ran.
    passed = os.environ.get("FAKE_UNAME", "Darwin arm64") == "Darwin arm64"
    info = {"strategy": os.environ.get("FAKE_ROUTING_STRATEGY", "remote")}
    if os.environ.get("FAKE_ROUTING_CACHED"):
        info["cachedRemotely"] = True
    if not os.environ.get("FAKE_ROUTING_STATUS"):
        # FAKE_ROUTING_STATUS stands for a probe that did not build: it reports no result.
        lines.append({"id": {"testResult": {"label": "//tools/ci:remote_macos_probe"}},
                      "testResult": {"status": "PASSED" if passed else "FAILED",
                                     "executionInfo": info}})
    status = int(os.environ.get("FAKE_ROUTING_STATUS", "0" if passed else "3"))
elif "--nobuild" in args:
    # The lane probe sees FAKE_EXECUTOR, the probe without the config FAKE_PLAIN_EXECUTOR.
    executor = os.environ.get("FAKE_EXECUTOR" if "--config=remote-macos" in args
                              else "FAKE_PLAIN_EXECUTOR")
    options = [{"optionName": "spawn_strategy", "optionValue": "remote"}]
    if executor:
        options.append({"optionName": "remote_executor", "optionValue": executor})
    label = "original" if os.environ.get("FAKE_NO_CANONICAL") else "canonical"
    lines.append({"id": {"structuredCommandLine": {"commandLineLabel": label}},
                  "structuredCommandLine": {"sections": [{"optionList": {"option": options}}]}})
    status = int(os.environ.get("FAKE_PREFLIGHT_STATUS", "0"))
else:
    phase = "gpu" if "--config=remote-macos-gpu" in args else "native"
    status = int(os.environ.get("FAKE_STATUS_" + phase.upper(), "0"))
    if os.environ.get("FAKE_KEEP_RUNNING_" + phase.upper()):
        # A cold cache: actions keep completing, more slowly than the budget allows. An
        # interrupt ends the invocation as Bazel does, with its build events written.
        import signal, time
        stopped = []
        signal.signal(signal.SIGINT, lambda *_: stopped.append(1))
        with open(os.environ["FAKE_PID_FILE"], "w") as handle:
            handle.write(str(os.getpid()))
        start, done = time.monotonic(), 0
        while not stopped and time.monotonic() - start < 40:
            done += 1
            print("[%d / 24,130] %d / 841 tests; Testing //fake:t; 3s remote" % (done, done // 10),
                  flush=True)
            time.sleep(0.05)
        with open(next(a.split("=", 1)[1] for a in args
                       if a.startswith("--build_event_json_file=")), "w") as handle:
            for line in ({"id": {"buildFinished": {}},
                          "finished": {"exitCode": {"name": "INTERRUPTED", "code": 8}}},
                         {"id": {"buildMetrics": {}}, "buildMetrics": {"actionSummary": {
                             "actionsExecuted": "5", "actionCacheStatistics": {"hits": 7}}}}):
                handle.write(json.dumps(line) + "\n")
        sys.exit(8)
    label = "//fake:%s_test" % phase
    result = {}
    if os.environ.get("FAKE_TESTLOGS"):
        # The test's outputs as Bazel downloads them, and one it names but did not download.
        directory = os.path.join(os.environ["FAKE_TESTLOGS"], "execroot", "_main", "bazel-out",
                                 "darwin_arm64-fastbuild", "testlogs", "fake", phase + "_test")
        os.makedirs(os.path.join(directory, "test.outputs"), exist_ok=True)
        files = {"test.log": "%s log\n" % phase, "test.xml": "<testsuites/>\n",
                 "test.outputs/outputs.zip": "%s images\n" % phase}
        for name, text in files.items():
            with open(os.path.join(directory, name), "w") as handle:
                handle.write(text)
        result = {"status": "FAILED" if status else "PASSED", "testActionOutput": [
            {"name": name.replace("/", "__"), "uri": "file://" + os.path.join(directory, name)}
            for name in [*files, "test.outputs_manifest/MANIFEST"]]}
    if os.environ.get("FAKE_SKIPPED"):
        # A test incompatible with the platform, as Bazel skips it.
        # The phase's tag filters select it: the GPU phase requires the GPU resource tag.
        tags = ["resources:local_gpu:1"] if phase == "gpu" else []
        lines += [{"id": {"targetConfigured": {"label": "//fake:linux_only_test"}},
                   "configured": {"targetKind": "cc_test rule", "testSize": "SMALL",
                                  "tag": tags}},
                  {"id": {"targetCompleted": {"label": "//fake:linux_only_test",
                                              "configuration": {"id": "c"}}},
                   "aborted": {"reason": "SKIPPED",
                               "description": "Target //fake:linux_only_test build was skipped."}},
                  # One the phase's tag filters leave out, which Bazel does not count.
                  {"id": {"targetConfigured": {"label": "//fake:browser_test"}},
                   "configured": {"targetKind": "js_test rule", "testSize": "MEDIUM",
                                  "tag": ["launches-browser"] + tags}},
                  {"id": {"targetCompleted": {"label": "//fake:browser_test",
                                              "configuration": {"id": "c"}}},
                   "aborted": {"reason": "SKIPPED"}}]
    if os.environ.get("FAKE_REMOTE_ERROR_" + phase.upper()):
        # An action the executor failed, as Bazel reports it: its exit code is REMOTE_ERROR.
        status = 34
        lines.append({"id": {"actionCompleted": {"label": "//fake:lib", "primaryOutput": "x"}},
                      "action": {"success": False, "exitCode": 34, "type": "CppCompile",
                                 "failureDetail": {"message": "Remote Execution Failure"}}})
    lines += [{"id": {"targetCompleted": {"label": label}}, "completed": {"success": True}},
              {"id": {"testResult": {"label": label}}, "testResult": result},
              {"id": {"testSummary": {"label": label}},
               "testSummary": {"overallStatus": "FAILED" if status else "PASSED"}},
              {"id": {"buildFinished": {}},
               "finished": {"exitCode": {"name": "X", "code": status}}},
              {"id": {"buildMetrics": {}}, "buildMetrics": {
                  "actionSummary": {"actionsExecuted": "7", "runnerCount": [
                      {"name": "total", "count": 9}, {"name": "remote", "count": 7}]},
                  "timingMetrics": {"wallTimeInMs": "1500"}}}]
with open(events, "w") as handle:
    handle.write("".join(json.dumps(line) + "\n" for line in lines))
sys.exit(status)
'''


def _collect(case, testlogs, cwd=None):
    """What the workflow's collector uploads when its `testlogs` input is `testlogs` (GitHub
    passes an omitted input as empty), run in `cwd` or an empty directory."""
    action = _read(COLLECTOR[2:] + "/action.yml")
    body = re.search(r"(?m)^      run: \|\n((?:(?:        .*)?\n)+)", action).group(1)
    directory = tempfile.TemporaryDirectory()
    case.addCleanup(directory.cleanup)
    result = subprocess.run(["bash", "-c", textwrap.dedent(body)], cwd=cwd or directory.name,
                            env=_environ(RUNNER_TEMP=directory.name, TESTLOGS=testlogs),
                            capture_output=True, text=True, check=False)
    case.assertEqual(0, result.returncode, result.stdout + result.stderr)
    artifacts = Path(directory.name) / "bazel-test-artifacts"
    return {path.relative_to(artifacts).as_posix(): path.read_text(encoding="utf-8")
            for path in artifacts.rglob("*") if path.is_file()}


class LaneRunTest(unittest.TestCase):
    """Runs the lane end to end with a stand-in Bazel."""

    @classmethod
    def setUpClass(cls):
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        script = cls.executables.path / "fake_bazel.py"
        script.write_text(_FAKE_BAZEL, encoding="utf-8")
        cls.bazel = str(cls.executables.write_python_entry("bazelisk", script))

    def _run(self, *arguments, **environment):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        out = Path(directory.name) / "out"
        summary_file = Path(directory.name) / "step-summary.md"
        environ = _environ(FALLBACK="false", AFFECTED="//fake:all",
                           FAKE_LOG=str(Path(directory.name) / "calls"),
                           GITHUB_STEP_SUMMARY=str(summary_file),
                           BAZEL_HEARTBEAT_INTERVAL_SECONDS="1", **environment)
        # The fake Bazel's output tree stands for the lane's own output root.
        root = ["--output-user-root", environ["FAKE_TESTLOGS"]] if "FAKE_TESTLOGS" in environ \
            else []
        result = subprocess.run(
            [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--out", str(out),
             "--bazel", self.bazel, *root, *arguments], cwd=_root(), env=environ,
            capture_output=True,
            text=True, check=False)
        calls = (Path(directory.name) / "calls").read_text(encoding="utf-8").splitlines()
        return result, out, summary_file, calls

    def _assert_stopped(self, result, out, calls, status, message):
        self.assertEqual(status, result.returncode, result.stdout + result.stderr)
        bazel = [call for call in calls if not call.endswith("shutdown")]
        self.assertLessEqual(len(bazel), 3, "only the preflight probes may run")
        self.assertTrue(calls[-1].endswith("shutdown"), "the lane leaves no Bazel server")
        self.assertIn(message, result.stdout)
        self.assertIn(message, (out / "report.md").read_text(encoding="utf-8"))
        self.assertEqual([], sorted(path.name for path in (out / "preflight").glob("*.json")))
        self.assertNotIn("invalid", result.stdout + result.stderr)

    def test_unconfigured_runner_fails_fast_without_building(self):
        result, out, summary_file, calls = self._run()
        self._assert_stopped(result, out, calls, 2,
                             "runner is not configured for the remote macOS lane")
        self.assertIn("build:remote-macos-base", summary_file.read_text(encoding="utf-8"))

    def test_executor_shared_with_a_build_without_the_config_stops_the_lane(self):
        endpoint = "grpcs://default-executor.invalid"
        result, out, _, calls = self._run(FAKE_EXECUTOR=endpoint, FAKE_PLAIN_EXECUTOR=endpoint)
        self._assert_stopped(result, out, calls, 2,
                             "resolves to the same remote executor as a build without it")

    def test_failed_preflight_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_PREFLIGHT_STATUS="37")
        self._assert_stopped(result, out, calls, 37, "preflight Bazel command failed (exit 37)")
        # The probe without the config, a build of no targets, does not run after a failure.
        self.assertEqual(1, len(calls) - 1, calls)
        self.assertIn("--config=remote-macos", calls[0])

    def test_a_repository_rule_failure_on_the_client_is_named(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_REPOSITORY_FAILURE="1")
        self._assert_stopped(result, out, calls, 37, "a repository rule failed on this runner "
                             "before anything ran remotely (exit 37)")
        self.assertEqual(1, len(calls) - 1, calls)

    def test_a_runner_rc_that_empties_a_client_variable_does_not_break_repositories(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_RC_EMPTIES_ACTION_ENV="1")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(6, len(calls))
        for call in calls[:-1]:
            with self.subTest(call=call[:60]):
                self.assertIn(lane.CLIENT_REPO_ENV, call.split())

    def test_executor_that_is_not_macos_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_UNAME="Linux aarch64")
        self._assert_stopped(result, out, calls, 2,
                             "reported a system other than macOS arm64 (exit 3)")

    def test_routing_probe_that_did_not_execute_remotely_is_not_proven(self):
        # A result kept by a persistent output root, or one that ran on the runner, proves nothing.
        for environment in ({"FAKE_ROUTING_CACHED": "1"}, {"FAKE_ROUTING_STRATEGY": "local"}):
            with self.subTest(**environment):
                result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                                  **environment)
                self._assert_stopped(result, out, calls, 2, "the routing probe is not proven")

    def test_routing_probe_that_cannot_run_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_ROUTING_STATUS="1")
        self._assert_stopped(result, out, calls, 2, "did not build or run on the configured "
                             "executor (exit 1)")
        self.assertNotIn("other than macOS arm64", result.stdout)

    def test_unreadable_preflight_is_not_reported_as_unconfigured(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_NO_CANONICAL="1")
        self._assert_stopped(result, out, calls, 2, "build event file could not be read")
        self.assertNotIn("not configured", result.stdout)

    def test_configured_runner_runs_both_phases_and_never_prints_the_endpoint(self):
        endpoint = "grpcs://private-executor.invalid:50051"
        result, out, summary_file, calls = self._run(FAKE_EXECUTOR=endpoint)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(6, len(calls))
        self.assertIn("--config=remote-macos-gpu --jobs=4", calls[4])
        self.assertTrue(calls[5].endswith("shutdown"))
        outputs = {"stdout": result.stdout + result.stderr,
                   "summary": summary_file.read_text(encoding="utf-8"),
                   "json": (out / "shadow-summary.json").read_text(encoding="utf-8"),
                   "report": (out / "report.md").read_text(encoding="utf-8")}
        for name, text in outputs.items():
            with self.subTest(output=name):
                self.assertNotIn("private-executor", text)
        self.assertEqual([], sorted(path.name for path in (out / "preflight").glob("*.json")))
        summary = json.loads(outputs["json"])
        for phase in lane.PHASES:
            entry = summary["phases"][phase]
            self.assertEqual(0, entry["status"])
            self.assertEqual({"total": 1, "passed": 1, "flaky": 0, "failed": 0, "not_run": 0,
                              "skipped": 0, "executed": 1, "cached": 0}, entry["tests"])
            self.assertEqual({"remote": 0, "own": 0}, entry["failed_actions"])
            self.assertEqual({"remote": 7}, entry["runners"])
            self.assertEqual(1.5, entry["wall_time_seconds"])
        self.assertIn("| native | 0 |", outputs["report"])

    def test_a_phase_past_the_budget_is_stopped_and_the_lane_still_reports(self):
        import contextlib
        import io
        from unittest import mock
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        out, pid_file, summary_file = root / "out", root / "pid", root / "step-summary.md"
        environ = _environ(FAKE_LOG=str(root / "calls"), FAKE_KEEP_RUNNING_NATIVE="1",
                           FAKE_PID_FILE=str(pid_file), GITHUB_STEP_SUMMARY=str(summary_file),
                           BAZEL_HEARTBEAT_INTERVAL_SECONDS="1")
        args = lane.parse_args(["--out", str(out), "--fallback", "true", "--bazel", self.bazel,
                                "--budget-minutes", "60"], {})
        real = lane.run_phase

        def gated(command, environment, console, **kwargs):
            # The budget runs out once the phase is running and has completed actions.
            kwargs.update(poll=0.2, limit=10 ** 12, clock=_GatedClock(
                lambda: pid_file.exists() and lane.progress(lane._tail(console)) is not None))
            return real(command, environment, console, **kwargs)

        with mock.patch.object(lane, "_preflight", return_value=(None, 0)), \
                mock.patch.object(lane, "run_phase", side_effect=gated), \
                contextlib.redirect_stdout(io.StringIO()):
            status = lane.run(args, environ)
        self.assertEqual(lane.OUT_OF_TIME_STATUS, status)
        # A failure status, so the upload steps keyed on the lane step's outcome run.
        self.assertNotEqual(0, status)
        calls = (root / "calls").read_text(encoding="utf-8").splitlines()
        # The native phase, a shutdown that completes its build events, the final shutdown; the
        # gpu phase never starts.
        self.assertEqual(3, len(calls), calls)
        self.assertIn("--config=remote-macos ", calls[0] + " ")
        self.assertEqual(["shutdown"] * 2, [call.split()[-1] for call in calls[1:]])
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        native = summary["phases"]["native"]
        self.assertEqual(lane.OUT_OF_TIME, native["outcome"])
        self.assertEqual(lane.OUT_OF_TIME_STATUS, native["status"])
        self.assertIsNone(summary["phases"]["gpu"])
        self.assertIn("budget ran out", summary["problem"])
        counts = native["progress"]
        self.assertEqual(24130, counts["actions_total"])
        self.assertEqual(841, counts["tests_total"])
        self.assertGreater(counts["actions_done"], 0)
        self.assertGreater(native["actions_per_minute"], 0)
        self.assertEqual((5, 7), (native["actions_executed"], native["action_cache_hits"]))
        for text in ((out / "report.md").read_text(encoding="utf-8"),
                     summary_file.read_text(encoding="utf-8")):
            self.assertIn("native stopped (out of time)", text)
            self.assertIn("/ 24130 actions", text)
            self.assertIn("actions per minute", text)
            self.assertIn("| gpu | - | not run |", text)
        self.assertFalse(_signallable(int(pid_file.read_text())))

    def test_a_preflight_or_routing_probe_out_of_time_is_labelled_and_exits_124(self):
        import contextlib
        import io
        from unittest import mock
        out = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, out)
        probes = [(0, "grpcs://executor.invalid", None), (0, "", None)]
        cases = {
            "preflight": [mock.patch.object(lane, "_run_captured",
                                            return_value=(lane.OUT_OF_TIME_STATUS, "", True))],
            "routing probe": [mock.patch.object(lane, "_probe", side_effect=list(probes)),
                              mock.patch.object(lane, "run_phase", return_value=(
                                  lane.OUT_OF_TIME_STATUS, lane.OUT_OF_TIME))]}
        for name, patches in cases.items():
            with self.subTest(command=name):
                args = lane.parse_args(["--out", str(out / name.replace(" ", "-")), "--fallback",
                                        "true", "--budget-minutes", "60"], {})
                with contextlib.ExitStack() as stack:
                    for patch in patches + [mock.patch.object(lane, "_shutdown")]:
                        stack.enter_context(patch)
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    status = lane.run(args, {})
                self.assertEqual(lane.OUT_OF_TIME_STATUS, status)
                summary = json.loads((Path(args.out) / "shadow-summary.json").read_text())
                self.assertIn("%s was stopped: out of time" % name, summary["problem"])

    def test_a_budget_that_is_not_reached_changes_nothing(self):
        result, out, _, calls = self._run("--budget-minutes", "60",
                                          FAKE_EXECUTOR="grpcs://executor.invalid")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(6, len(calls))
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        self.assertNotIn("problem", summary)
        for phase in lane.PHASES:
            self.assertNotIn("outcome", summary["phases"][phase])
            self.assertEqual(0, summary["phases"][phase]["status"])
        self.assertNotIn("out of time", result.stdout)

    def test_a_successful_run_with_skipped_targets_prints_no_warning(self):
        result, out, summary_file, _ = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                                 FAKE_SKIPPED="1")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertNotIn("::warning::", result.stdout + result.stderr)
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        for phase in lane.PHASES:
            entry = summary["phases"][phase]
            self.assertEqual({"succeeded": 1, "failed": 0, "skipped": 2, "aborted": 0},
                             entry["targets"])
            self.assertEqual((2, 1, 1), (entry["tests"]["total"], entry["tests"]["passed"],
                                         entry["tests"]["skipped"]))
        self.assertIn("| native | 0 | X (0) | 1.5 | 1 | 0 | 2 | 2 | 1 |",
                      summary_file.read_text(encoding="utf-8"))

    def test_a_remote_execution_error_is_told_apart_from_a_build_break(self):
        result, out, summary_file, _ = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                                 FAKE_REMOTE_ERROR_NATIVE="1")
        self.assertEqual(34, result.returncode, result.stdout + result.stderr)
        self.assertIn("::warning::macOS remote shadow lane: in the native phase, 1 actions failed "
                      "with a remote execution error (executor side), 0 with their own exit "
                      "status.", result.stdout)
        self.assertNotIn("in the gpu phase", result.stdout)
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        self.assertEqual({"remote": 1, "own": 0}, summary["phases"]["native"]["failed_actions"])
        self.assertIn("- native: 1 actions failed with a remote execution error (executor side)",
                      summary_file.read_text(encoding="utf-8"))

    def test_failed_phase_is_reported_and_the_other_phase_still_runs(self):
        result, out, summary_file, calls = self._run(
            FAKE_EXECUTOR="grpcs://executor.invalid", FAKE_STATUS_NATIVE="3")
        self.assertEqual(3, result.returncode)
        self.assertEqual(6, len(calls))
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        self.assertEqual(["//fake:native_test"], summary["phases"]["native"]["failed_tests"])
        self.assertEqual(0, summary["phases"]["gpu"]["status"])
        self.assertIn("::warning::macOS remote shadow lane: the native phase exited 3",
                      result.stdout)
        self.assertIn("//fake:native_test", summary_file.read_text(encoding="utf-8"))

    def _collected(self, out):
        """What the workflow's collector uploads from the script's staged tree."""
        return _collect(self, str(out / lane.ARTIFACTS))

    def _run_with_outputs(self, **environment):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        testlogs = Path(directory.name) / "output-base"
        # A failed result an earlier run left in the lane's persistent output root.
        stale = testlogs / "execroot/_main/bazel-out/darwin_arm64-fastbuild/testlogs/fake/old_test"
        (stale / "test.outputs").mkdir(parents=True)
        (stale / "test.log").write_text("an earlier run\n", encoding="utf-8")
        (stale / "test.outputs" / "outputs.zip").write_text("old images\n", encoding="utf-8")
        return self._run(FAKE_EXECUTOR="grpcs://executor.invalid", FAKE_TESTLOGS=str(testlogs),
                         **environment)

    @staticmethod
    def _phase_outputs(phase):
        prefix = "%s/darwin_arm64-fastbuild/fake/%s_test/" % (phase, phase)
        return {prefix + "test.log": "%s log\n" % phase,
                prefix + "test.xml": "<testsuites/>\n",
                prefix + "test.outputs/outputs.zip": "%s images\n" % phase}

    def test_each_failed_phase_stages_its_failed_tests_outputs_for_the_collector(self):
        for failing in (("native", "gpu"), ("native",), ("gpu",)):
            with self.subTest(failing=failing):
                result, out, _, _ = self._run_with_outputs(**{
                    "FAKE_STATUS_" + phase.upper(): "3" for phase in failing})
                self.assertEqual(3, result.returncode, result.stdout + result.stderr)
                expected = {}
                for phase in failing:
                    expected.update(self._phase_outputs(phase))
                self.assertEqual(expected, self._collected(out))
                for phase in lane.PHASES:
                    self.assertRegex(result.stdout, r"staged %d output files \(\d+ bytes\) for %s\."
                                     % (3 if phase in failing else 0, phase))
                self.assertNotIn("staged no output files", result.stdout)

    def test_nothing_is_staged_when_every_test_passes(self):
        result, out, _, _ = self._run_with_outputs()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual([], [path for path in (out / lane.ARTIFACTS).rglob("*")
                              if path.is_file()])
        self.assertEqual({}, self._collected(out))
        for phase in lane.PHASES:
            self.assertIn("staged 0 output files (0 bytes) for %s." % phase, result.stdout)
        self.assertNotIn("::warning::", result.stdout)

    def test_empty_selection_fails_before_bazel(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        result = subprocess.run(
            [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--out",
             directory.name, "--bazel", self.bazel, "--affected", "  "],
            cwd=_root(), capture_output=True, text=True, check=False,
            env=_environ(FALLBACK="false", FAKE_LOG=str(Path(directory.name) / "calls")))
        self.assertEqual(2, result.returncode)
        self.assertIn("Empty target selection", result.stdout)
        self.assertFalse((Path(directory.name) / "calls").exists())


# Writes Bazel-like progress lines to a console log, as the watchdog's tee does: a starved executor
# (the counter frozen, only the elapsed time growing), or a command that keeps completing actions
# and starts a child. It runs as a real process, to prove how a stop reaches its process group;
# when the guard runs is tested without real time (_ScriptedPhase).
_FAKE_CONSOLE = r'''
import os, signal, sys, time
mode, console, step = sys.argv[1], sys.argv[2], float(sys.argv[3])
interrupts = []
signal.signal(signal.SIGINT, lambda *_: interrupts.append(1))
def line(done, total, elapsed):
    with open(console, "a") as handle:
        handle.write("[%d / %d] Compiling a.cc; %.2fs remote, remote-cache ... (3 actions, "
                     "0 running)\n" % (done, total, elapsed))
start = time.monotonic()
if mode == "starved":
    # Bounded, so a guard that never fires fails the test instead of hanging it.
    while len(interrupts) < (3 if len(sys.argv) > 4 else 1) and time.monotonic() - start < 30:
        line(137, 150, time.monotonic() - start)
        time.sleep(step)
    sys.exit(8)
if mode == "spawning":
    # Records its own and a child's pid, then keeps completing actions until it is stopped.
    import subprocess
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    with open(sys.argv[4], "w") as handle:
        handle.write("%d %d" % (os.getpid(), child.pid))
    done = 0
    while not interrupts and time.monotonic() - start < 30:
        done += 1
        line(done, 100000, time.monotonic() - start)
        time.sleep(step)
    sys.exit(8)
'''


class _StepClock:
    """A clock that moves only when the scripted phase advances it."""

    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class _ScriptedPhase:
    """A phase command driven by a script instead of real time. Each wait that times out appends
    the script's next console line (None appends nothing), advances the clock by the wait's
    timeout and raises TimeoutExpired, as a running command would; once the script is used up,
    the command exits 0."""

    pid = None

    def __init__(self, console, script, clock):
        self.console, self.script, self.clock = console, list(script), clock
        self.waits = 0

    def wait(self, timeout=None):
        if not self.script:
            return 0
        line = self.script.pop(0)
        if line is not None:
            with open(self.console, "a", encoding="utf-8") as handle:
                handle.write(line + "\n")
        self.waits += 1
        self.clock.now += timeout
        raise subprocess.TimeoutExpired("phase", timeout)


def _progress(done, total, elapsed=3):
    return "[%d / %d] Compiling a.cc; %ds remote, remote-cache ... (3 actions, 0 running)" % (
        done, total, elapsed)


class StarvationGuardTest(unittest.TestCase):
    """The lane stops a phase whose executor completes nothing, and only that."""

    LIMIT = 1.0
    # The scripted runs: the guard's bound and the poll interval, on the step clock.
    BOUND, POLL = 10, 1

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.console = self.directory / "console.log"
        self.script = self.directory / "fake_console.py"
        self.script.write_text(_FAKE_CONSOLE, encoding="utf-8")

    def _phase(self, mode, step, *extra):
        import contextlib
        import io
        command = [sys.executable, str(self.script), mode, str(self.console), str(step), *extra]
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            status, starved = lane.run_phase(command, _environ(), self.console,
                                             limit=self.LIMIT, poll=0.05, grace=(2, 2, 2))
        return status, starved, printed.getvalue()

    def _scripted(self, script):
        """(status, stop, the step clock's time at the end, stop calls, printed) of the wait loop
        over a scripted phase."""
        import contextlib
        import io
        from unittest import mock
        clock = _StepClock()
        process = _ScriptedPhase(self.console, script, clock)
        with mock.patch.object(lane, "_stop") as stop, \
                contextlib.redirect_stdout(io.StringIO()) as printed:
            status, stopped = lane._wait_phase(process, self.console, self.BOUND, self.POLL,
                                               clock, (2, 2, 2), None)
        return status, stopped, clock.now, stop.call_count, printed.getvalue()

    def test_frozen_counter_with_a_growing_elapsed_time_is_starved_at_the_bound(self):
        # The first progress line arrives on the first poll; the counter never moves after it.
        script = [_progress(137, 150, elapsed) for elapsed in range(1, 40)]
        status, stopped, now, stops, printed = self._scripted(script)
        self.assertEqual((lane.STARVED_STATUS, lane.STARVED), (status, stopped))
        # Stopped on exactly the poll that is BOUND after the first sight of the counter.
        self.assertEqual(self.POLL + self.BOUND, now)
        self.assertEqual(1, stops)
        self.assertIn(lane.STARVED, printed)
        # The elapsed time kept changing, so a whole-line comparison saw progress throughout.
        lines = [line for line in self.console.read_text().splitlines() if line.startswith("[")]
        self.assertGreater(len(set(lines)), 5)
        self.assertEqual({(137, 150)}, {lane.progress(line) for line in lines})

    def test_slow_but_steady_progress_is_not_starved(self):
        # The counter advances every 3 polls, 3 s on the step clock, well inside the 10 s bound.
        script = []
        for done in range(10):
            script += [_progress(done, 10), None, None]
        status, stopped, now, stops, _ = self._scripted(script)
        self.assertEqual((0, None, 0), (status, stopped, stops))
        self.assertEqual(30 * self.POLL, now)

    def test_one_long_action_inside_the_bound_is_not_starved(self):
        # One action holds the counter for one poll less than the bound, then the build ends.
        held = self.BOUND // self.POLL - 1
        script = [_progress(5, 10)] + [None] * held + [_progress(10, 10)]
        status, stopped, _, stops, _ = self._scripted(script)
        self.assertEqual((0, None, 0), (status, stopped, stops))
        # One poll more and the same action is starved.
        status, stopped, _, stops, _ = self._scripted(
            [_progress(5, 10)] + [None] * (held + 1) + [_progress(10, 10)])
        self.assertEqual((lane.STARVED, 1), (stopped, stops))

    def test_a_client_that_ignores_the_first_interrupts_is_still_stopped(self):
        # A real process: the interrupts and the kill reach its process group.
        status, starved, _ = self._phase("starved", 0.05, "stubborn")
        self.assertEqual((lane.STARVED_STATUS, lane.STARVED), (status, starved))

    def test_starved_phase_skips_the_next_phase_and_is_reported(self):
        from unittest import mock
        calls = []

        def fake_phase(command, environment, console, **_):
            calls.append(command)
            return lane.STARVED_STATUS, lane.STARVED

        args = lane.parse_args(["--out", str(self.directory / "out"), "--fallback", "true"], {})
        with mock.patch.object(lane, "_preflight", return_value=(None, 0)), \
                mock.patch.object(lane, "run_phase", side_effect=fake_phase), \
                mock.patch.object(lane, "_shutdown") as shutdown:
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()):
                status = lane.run(args, {})
        self.assertEqual(lane.STARVED_STATUS, status)
        # Once after the stopped phase, before its build events are read, and once at the end.
        self.assertEqual(2, shutdown.call_count)
        self.assertEqual(1, len(calls))
        summary = json.loads((self.directory / "out" / "shadow-summary.json").read_text())
        self.assertEqual(lane.STARVED, summary["phases"]["native"]["outcome"])
        self.assertIsNone(summary["phases"]["gpu"])
        report = (self.directory / "out" / "report.md").read_text()
        self.assertIn(lane.STARVED, report)
        self.assertIn("| gpu | - | not run |", report)

    def test_routing_probe_runs_under_its_own_bound(self):
        from unittest import mock
        seen = {}

        def fake_phase(command, environment, console, limit, **_):
            seen.update(limit=limit, console=console)
            return lane.STARVED_STATUS, lane.STARVED

        args = lane.parse_args(["--out", str(self.directory / "out")], {})
        with mock.patch.object(lane, "run_phase", side_effect=fake_phase), \
                mock.patch.object(lane, "_shutdown") as shutdown:
            problem, status = lane._routing_problem(args, {})
        self.assertEqual(2, status)
        # The server is shut down before the stopped probe's build events are read.
        self.assertEqual(1, shutdown.call_count)
        self.assertEqual(lane.ROUTING_STARVED_SECONDS, seen["limit"])
        self.assertLessEqual(lane.ROUTING_STARVED_SECONDS, 10 * 60)
        self.assertIn("not scheduled within 10 minutes", problem)
        self.assertEqual("routing.log", Path(seen["console"]).name)

    def test_guard_rules(self):
        guard = lane.StarvationGuard(0, limit=10)
        self.assertFalse(guard.starved(None, 100))  # Before the first progress line.
        self.assertFalse(guard.starved((5, 9), 101))
        self.assertFalse(guard.starved((5, 9), 110))
        self.assertTrue(guard.starved((5, 9), 111))
        self.assertFalse(guard.starved((6, 9), 112))  # Advancing resets the clock.
        self.assertFalse(guard.starved((9, 9), 200))  # Nothing left to run.
        self.assertEqual((1234, 5678), lane.progress("x\n[1,234 / 5,678] Testing //a:b\n"))
        # A runner rc may turn on --show_timestamps; the guard must still read the counter.
        self.assertEqual((7, 9), lane.progress("(12:34:56) [7 / 9] Compiling a.cc; 3s remote\n"))
        self.assertEqual((7, 9), lane.progress("(12:34:56.789) [7 / 9] Testing //a:b\n"))
        self.assertIsNone(lane.progress("Loading: 3 packages loaded\n"))

    def test_the_budget_stops_a_progressing_phase_and_leaves_no_process(self):
        import contextlib
        import io
        pids = self.directory / "pids"
        command = [sys.executable, str(self.script), "spawning", str(self.console), "0.05",
                   str(pids)]
        clock = _GatedClock(lambda: pids.exists() and lane.progress(lane._tail(self.console)))
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            # The deadline is half a second ahead on the stopped clock, so each poll is cut to
            # half a second; an uncut poll of 60 seconds would outlast the stand-in, which ends
            # on its own after 30 seconds.
            status, stop = lane.run_phase(command, _environ(), self.console, limit=10 ** 12,
                                          poll=60, grace=(5, 5, 5), clock=clock,
                                          deadline=clock() + 0.5)
        self.assertEqual((lane.OUT_OF_TIME_STATUS, lane.OUT_OF_TIME), (status, stop))
        self.assertIn("the lane's budget ran out", printed.getvalue())
        own, child = (int(value) for value in pids.read_text().split())
        self.assertFalse(_signallable(own))
        self.assertFalse(_signallable(child), "the stopped command left a process behind")

    def test_starvation_still_stops_first_when_nothing_completes(self):
        # The budget runs out on the very poll that the counter has been frozen for the bound.
        import contextlib
        import io
        from unittest import mock
        clock = _StepClock()
        process = _ScriptedPhase(self.console, [_progress(5, 10)] * 40, clock)
        with mock.patch.object(lane, "_stop"), contextlib.redirect_stdout(io.StringIO()):
            status, stop = lane._wait_phase(process, self.console, self.BOUND, self.POLL, clock,
                                            (2, 2, 2), self.POLL + self.BOUND)
        self.assertEqual((lane.STARVED_STATUS, lane.STARVED), (status, stop))
        self.assertEqual(self.POLL + self.BOUND, clock.now)

    def test_a_preflight_command_is_stopped_at_the_deadline(self):
        import contextlib
        import io
        command = [sys.executable, "-c", "import time; time.sleep(60)"]
        with contextlib.redirect_stdout(io.StringIO()):
            status, _, stopped = lane._run_captured(command, _environ(),
                                                    self.directory / "probe.log",
                                                    time.monotonic() + 1, grace=(5, 5, 5))
        self.assertEqual((lane.OUT_OF_TIME_STATUS, True), (status, stopped))
        self.assertFalse((self.directory / "probe.log").exists())
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual((3, "", False), lane._run_captured(
                [sys.executable, "-c", "raise SystemExit(3)"], _environ(),
                self.directory / "probe.log", time.monotonic() + 600))

    def test_a_stop_ends_what_the_command_leaves_behind(self):
        # A shell background job ignores interrupts, as the stall watchdog's heartbeat does.
        pid_file = self.directory / "background"
        process = subprocess.Popen(
            ["bash", "-c", 'sleep 60 & echo $! > "$1"; sleep 60', "bash", str(pid_file)],
            start_new_session=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        bound = time.monotonic() + 60
        while not (pid_file.exists() and pid_file.read_text().strip()) and \
                time.monotonic() < bound:
            time.sleep(0.05)
        background = int(pid_file.read_text())
        lane._stop(process, grace=(30, 30, 30))
        self.assertIsNotNone(process.returncode)
        self.assertFalse(_signallable(background), "the interrupt-immune job was left running")

    def test_a_group_already_gone_is_reaped_without_a_kill(self):
        from unittest import mock
        process = subprocess.Popen([sys.executable, "-c", "pass"], start_new_session=True)
        sent = []

        def killpg(group, sig):
            sent.append(sig)
            raise ProcessLookupError

        with mock.patch.object(lane.os, "killpg", side_effect=killpg):
            lane._stop(process)
        self.assertEqual([signal.SIGINT], sent)
        self.assertIsNotNone(process.returncode)

    def test_a_group_with_nothing_the_lane_may_signal_is_not_waited_for(self):
        # EPERM: the group still has processes, none the lane may signal (exited ones waiting to
        # be reaped, as macOS reports them, or ones out of its reach). Nothing is left to stop, so
        # the lane neither waits out its bounds nor escalates, and says so once.
        import contextlib
        import io
        from unittest import mock
        sent = []

        def killpg(group, sig):
            sent.append(sig)
            raise PermissionError(1, "Operation not permitted")

        with mock.patch.object(lane.os, "killpg", side_effect=killpg), \
                mock.patch.object(lane.time, "sleep", side_effect=AssertionError("waited")), \
                contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertTrue(lane._group_gone(12345, 3600))
            lane._end_group(12345)
        self.assertEqual([0, 0], sent)
        self.assertEqual(2, printed.getvalue().count("has no process the lane may signal"))
        # The command itself is still waited for, so its exit status is known.
        process = subprocess.Popen([sys.executable, "-c", "pass"], start_new_session=True)
        sent.clear()
        with mock.patch.object(lane.os, "killpg", side_effect=killpg), \
                contextlib.redirect_stdout(io.StringIO()):
            lane._stop(process)
        self.assertEqual([signal.SIGINT], sent)
        self.assertEqual(0, process.returncode)

    def test_a_command_no_signal_reaches_does_not_hang_the_lane(self):
        import contextlib
        import io
        from unittest import mock
        process = mock.Mock(pid=12345)
        process.wait.side_effect = subprocess.TimeoutExpired("command", 1)
        with mock.patch.object(lane.os, "killpg",
                               side_effect=PermissionError(1, "Operation not permitted")), \
                contextlib.redirect_stdout(io.StringIO()) as printed:
            lane._stop(process)
        process.wait.assert_called_once_with(timeout=lane.STOP_KILL_SECONDS)
        self.assertIn("the stopped command did not exit within %d seconds and is left running"
                      % lane.STOP_KILL_SECONDS, printed.getvalue())

    def test_bound_exceeds_the_longest_test_timeout(self):
        # `long` (900 s) is the longest effective test timeout in the tree; see STARVED_SECONDS.
        self.assertGreater(lane.STARVED_SECONDS, 900)
        self.assertLessEqual(lane.STARVED_SECONDS, 30 * 60)


class BuildEventTest(unittest.TestCase):
    def test_effective_executor_reads_only_the_canonical_command_line(self):
        set_executor = _canonical([{"optionName": "remote_executor", "optionValue": "x"},
                                   {"optionName": "remote_executor", "optionValue": "y"}])
        original = dict(set_executor,
                        id={"structuredCommandLine": {"commandLineLabel": "original"}})
        self.assertEqual("y", lane.effective_executor(_events(original, set_executor)))
        self.assertEqual("", lane.effective_executor(_events(_canonical([
            {"optionName": "remote_cache", "optionValue": "x"}]))))
        self.assertEqual("", lane.effective_executor(_events(_canonical([
            {"optionName": "remote_executor", "optionValue": ""}]))))
        with self.assertRaises(ValueError):
            lane.effective_executor(_events(original))

    def test_probe_must_have_executed_remotely_in_this_invocation(self):
        def result(**fields):
            return {"id": {"testResult": {"label": lane.ROUTING_PROBE}}, "testResult": fields}
        remote = {"strategy": "remote"}
        self.assertTrue(lane.probe_executed_remotely(_events(
            result(status="PASSED", executionInfo=remote))))
        for fields in ({"status": "FAILED", "executionInfo": remote},
                       {"status": "PASSED", "executionInfo": dict(remote, cachedRemotely=True)},
                       {"status": "PASSED", "cachedLocally": True, "executionInfo": remote},
                       {"status": "PASSED", "executionInfo": {"strategy": "linux-sandbox"}}):
            with self.subTest(fields=fields):
                self.assertFalse(lane.probe_executed_remotely(_events(result(**fields))))
        self.assertFalse(lane.probe_executed_remotely(_events(_canonical([]))))

    def test_preflight_problem(self):
        ok = {"lane": 0, "plain": 0}
        self.assertIsNone(lane.preflight_problem(ok, {"lane": "a", "plain": ""}))
        self.assertIsNone(lane.preflight_problem(ok, {"lane": "a", "plain": "b"}))
        for statuses, executors, needle in (
                (ok, {"lane": "", "plain": ""}, "set no remote executor"),
                (ok, {"lane": "a", "plain": "a"}, "same remote executor"),
                (ok, {"lane": None, "plain": "b"}, "could not be read"),
                ({"lane": 0, "plain": 1}, {"lane": "a", "plain": "b"}, "exit 1")):
            with self.subTest(needle=needle):
                self.assertIn(needle, lane.preflight_problem(statuses, executors))

    def test_probe_readers_skip_fields_of_an_unexpected_type(self):
        junk = _events({"id": "x"}, {"id": {"testResult": "x"}}, {"id": ["x"]},
                       {"id": {"testResult": {"label": lane.ROUTING_PROBE}}, "testResult": "x"},
                       {"id": {"testResult": {"label": lane.ROUTING_PROBE}},
                        "testResult": {"status": "PASSED", "executionInfo": "remote"}})
        self.assertFalse(lane.probe_executed_remotely(junk))
        self.assertFalse(lane.probe_ran_and_failed(junk))

    def test_failed_actions_and_tests_that_never_ran_are_told_apart(self):
        # Action events in the shapes Bazel 8.8.2 wrote: a remote execution error (exit 34 and a
        # remoteExecution failure detail), an action that failed by its own exit status, and a
        # remote error known only by its exit code, as the executor's own failures report it.
        def action(label, exit_code, detail, success=False):
            return {"id": {"actionCompleted": {"label": label, "primaryOutput": "x"}},
                    "action": {"success": success, "exitCode": exit_code, "label": label,
                               "type": "CppCompile", "failureDetail": detail}}

        events = _events(
            action("//a:remote", 34, {"message": "refused",
                                      "remoteExecution": {"code": "CAPABILITIES_QUERY_FAILURE"}}),
            action("//a:own", 1, {"message": "error", "spawn": {"code": "NON_ZERO_EXIT"}}),
            action("//a:executor", 34, {"message": "Remote Execution Failure"}),
            action("//a:evicted", 39, {"message": "lost inputs"}),
            # A remote execution failure detail without a remote exit code is one too.
            action("//a:detail", 0,
                   {"message": "x", "remoteExecution": {"code": "EXECUTION_FAILED"}}),
            action("//a:ok", 0, None, success=True),
            {"id": {"actionCompleted": {"label": "//a:odd"}}, "action": "x"},
            *({"id": {"testSummary": {"label": "//a:t%d" % index}},
               "testSummary": {"overallStatus": status}}
              for index, status in enumerate(("FAILED_TO_BUILD", "NO_STATUS", "FAILED", "PASSED"))))
        summary = lane.summarize(events)
        self.assertEqual({"remote": 4, "own": 1}, summary["failed_actions"])
        self.assertEqual({"total": 4, "passed": 1, "flaky": 0, "failed": 1, "not_run": 2,
                          "skipped": 0, "executed": 0, "cached": 4}, summary["tests"])
        self.assertEqual(["//a:t2"], summary["failed_tests"])
        summary.update(status=1, wall_time_seconds=1.0)
        report = lane.report({"phases": {"native": summary}})
        self.assertIn("- native: 4 actions failed with a remote execution error (executor side), "
                      "1 with their own exit status", report)
        self.assertIn("| Failed | Not run |", report)
        self.assertIn("| 1 | 2 |", report)

    # Build events in the shapes Bazel 8.8.2 wrote for `bazel test --keep_going
    # --skip_incompatible_explicit_targets` of a passing test, a test and a genrule incompatible
    # with the platform, a genrule that fails and a test that depends on it. Bazel's own line:
    # "Executed 1 out of 3 tests: 1 test passes, 1 fails to build, and 1 was skipped."
    _CFG, _EXEC = {"id": "8fa4c4ac"}, {"id": "54191d9e"}
    _PROBE_EVENTS = (
        {"id": {"targetConfigured": {"label": "//:incompatible_test"}},
         "configured": {"targetKind": "sh_test rule", "testSize": "MEDIUM"}},
        {"id": {"targetConfigured": {"label": "//:incompatible_lib"}},
         "configured": {"targetKind": "genrule rule"}},
        {"id": {"targetConfigured": {"label": "//:broken"}},
         "configured": {"targetKind": "genrule rule"}},
        {"id": {"actionCompleted": {"configuration": _EXEC, "label": "//:broken",
                                    "primaryOutput": "bazel-out/aarch64-fastbuild/bin/b.txt"}},
         "action": {"exitCode": 1, "label": "//:broken", "type": "Genrule", "failureDetail": {
             "message": "bash failed", "spawn": {"code": "NON_ZERO_EXIT", "spawnExitCode": 1}}}},
        {"id": {"targetCompleted": {"configuration": _EXEC, "label": "//:broken"}},
         "completed": {"failureDetail": {"message": "bash failed",
                                         "spawn": {"code": "NON_ZERO_EXIT", "spawnExitCode": 1}}}},
        {"id": {"targetConfigured": {"label": "//:passes"}},
         "configured": {"targetKind": "sh_test rule", "testSize": "MEDIUM"}},
        {"id": {"targetConfigured": {"label": "//:needs_broken"}},
         "configured": {"targetKind": "sh_test rule", "testSize": "MEDIUM"}},
        {"id": {"targetCompleted": {"configuration": _CFG, "label": "//:passes"}},
         "completed": {"success": True, "tag": ["medium"], "testTimeout": "300s"}},
        {"id": {"targetCompleted": {"configuration": _CFG, "label": "//:needs_broken"}},
         "completed": {"failureDetail": {"message": "bash failed",
                                         "spawn": {"code": "NON_ZERO_EXIT", "spawnExitCode": 1}},
                       "tag": ["medium"]}},
        {"id": {"testResult": {"attempt": 1, "configuration": _CFG, "label": "//:passes",
                               "run": 1, "shard": 1}},
         "testResult": {"executionInfo": {"strategy": "linux-sandbox"}, "status": "PASSED"}},
        {"id": {"testSummary": {"configuration": _CFG, "label": "//:passes"}},
         "testSummary": {"overallStatus": "PASSED", "runCount": 1, "totalRunCount": 1}},
        {"id": {"targetCompleted": {"configuration": _CFG, "label": "//:incompatible_test"}},
         "aborted": {"description": "Target //:incompatible_test build was skipped.",
                     "reason": "SKIPPED"}},
        {"id": {"targetCompleted": {"configuration": _EXEC, "label": "//:incompatible_lib"}},
         "aborted": {"description": "Target //:incompatible_lib build was skipped.",
                     "reason": "SKIPPED"}},
        {"id": {"buildFinished": {}},
         "finished": {"exitCode": {"code": 1, "name": "BUILD_FAILURE"}}})

    def test_counts_equal_bazels_own_line_for_a_recorded_run(self):
        # The build events of a real Bazel 8.8.2 run on this repository (Linux execution, the
        # native phase's tag filters, --keep_going --skip_incompatible_explicit_targets), kept to
        # the fields the summary reads, configuration ids renamed. The run tested an alias of a
        # test and that test, a sharded test, two tests skipped as incompatible, a third one the
        # tag filters leave out, a test the tag filters leave out and a test_suite of one. Bazel
        # printed: "Executed 0 out of 5 tests: 3 tests pass and 2 were skipped."
        lines = _read("tools/testdata/macos_remote_shadow_build_events.json").splitlines()
        summary = lane.summarize(lines, lane.tag_filters("native"))
        self.assertEqual({"total": 5, "passed": 3, "flaky": 0, "failed": 0, "not_run": 0,
                          "skipped": 2, "executed": 0, "cached": 2}, summary["tests"])
        # Without the tag filters, every incompatible test would count as skipped.
        self.assertEqual(3, lane.summarize(lines)["tests"]["skipped"])

    def test_tag_filters_select_tests_as_bazel_does(self):
        native, gpu = lane.tag_filters("native"), lane.tag_filters("gpu")
        self.assertTrue(lane.selected_by_tags([], native))
        self.assertTrue(lane.selected_by_tags(["fuzz_target"], native))
        self.assertFalse(lane.selected_by_tags(["launches-browser"], native))
        self.assertFalse(lane.selected_by_tags(["resources:local_gpu:1"], native))
        self.assertFalse(lane.selected_by_tags([], gpu))
        self.assertTrue(lane.selected_by_tags(["resources:local_gpu:1", "variant"], gpu))
        self.assertFalse(lane.selected_by_tags(["resources:local_gpu:1", "perf"], gpu))
        self.assertTrue(lane.selected_by_tags(["anything"], ""))

    def test_skipped_targets_are_not_build_failures(self):
        summary = lane.summarize(_events(*self._PROBE_EVENTS))
        self.assertEqual({"succeeded": 1, "failed": 2, "skipped": 2, "aborted": 0},
                         summary["targets"])
        # Bazel: 3 tests, 1 passes, 1 fails to build, 1 was skipped; 1 executed.
        self.assertEqual({"total": 3, "passed": 1, "flaky": 0, "failed": 0, "not_run": 1,
                          "skipped": 1, "executed": 1, "cached": 0}, summary["tests"])
        self.assertEqual({"remote": 0, "own": 1}, summary["failed_actions"])
        # A stopped build's targets are not built, not failed; analysis failures are failures.
        stopped = lane.summarize(_events(
            {"id": {"targetCompleted": {"label": "//a:b"}}, "aborted": {"reason": "INCOMPLETE"}},
            {"id": {"targetCompleted": {"label": "//a:c"}},
             "aborted": {"reason": "ANALYSIS_FAILURE"}}))
        self.assertEqual({"succeeded": 0, "failed": 1, "skipped": 0, "aborted": 1},
                         stopped["targets"])
        self.assertIn("- native: 1 targets were not built because the build stopped",
                      lane.report({"phases": {"native": dict(stopped, status=1)}}))

    def test_an_event_holding_a_line_separator_character_stays_whole(self):
        # JSON may carry U+0085, U+2028 and U+2029 unescaped inside a string; str.splitlines
        # would cut such an event in two and lose it.
        events = [{"id": {"testSummary": {"label": "//a:t\u2028\u0085"}},
                   "testSummary": {"overallStatus": "PASSED"}},
                  {"id": {"buildFinished": {}}, "finished": {"exitCode": {"code": 0}}}]
        path = Path(tempfile.mkdtemp()) / "bep.json"
        self.addCleanup(shutil.rmtree, path.parent)
        path.write_text("".join(json.dumps(event, ensure_ascii=False) + "\n" for event in events),
                        encoding="utf-8")
        self.assertEqual(1, lane.summarize(lane._read_lines(path))["tests"]["total"])

    def test_a_phase_that_only_skipped_targets_prints_no_warning(self):
        events = [event for event in self._PROBE_EVENTS
                  if event["id"].get("targetCompleted", {}).get("label") not in (
                      "//:broken", "//:needs_broken") and "actionCompleted" not in event["id"]]
        summary = lane.summarize(_events(*events))
        self.assertEqual(0, summary["targets"]["failed"])
        args = lane.parse_args(["--out", tempfile.mkdtemp()], {})
        self.addCleanup(shutil.rmtree, args.out)
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            lane._stage_phase(args, _events(*events), "native", lane.MAX_ARTIFACT_TOTAL_BYTES, 0,
                              summary["targets"]["failed"])
        self.assertNotIn("::warning::", printed.getvalue())

    def test_summary_counts_targets_tests_cache_and_runners(self):
        events = _events(
            {"id": {"targetCompleted": {"label": "//a:lib"}}, "completed": {"success": True}},
            {"id": {"targetCompleted": {"label": "//a:bad"}},
             "completed": {"failureDetail": {"message": "x", "spawn": {"code": "NON_ZERO_EXIT"}}}},
            {"id": {"testResult": {"label": "//a:t1"}}, "testResult": {}},
            {"id": {"testResult": {"label": "//a:t2"}},
             "testResult": {"executionInfo": {"cachedRemotely": True}}},
            {"id": {"testResult": {"label": "//a:t3"}}, "testResult": {"cachedLocally": True}},
            {"id": {"testSummary": {"label": "//a:t1"}},
             "testSummary": {"overallStatus": "PASSED"}},
            {"id": {"testSummary": {"label": "//a:t2"}}, "testSummary": {"overallStatus": "FLAKY"}},
            {"id": {"testSummary": {"label": "//a:t3"}},
             "testSummary": {"overallStatus": "TIMEOUT"}},
            {"id": {"buildFinished": {}}, "finished": {"exitCode": {"name": "TESTS_FAILED",
                                                                         "code": 3}}},
            {"id": {"buildMetrics": {}}, "buildMetrics": {
                "actionSummary": {"actionsExecuted": "12", "actionCacheStatistics": {
                    "hits": 30, "misses": "4"}, "runnerCount": [
                        {"name": "total", "count": 20}, {"name": "remote", "count": 10},
                        {"name": "remote cache hit", "count": "8"}]},
                "timingMetrics": {"wallTimeInMs": "65432"}}})
        summary = lane.summarize(events)
        self.assertEqual({"succeeded": 1, "failed": 1, "skipped": 0, "aborted": 0},
                         summary["targets"])
        self.assertEqual({"total": 3, "passed": 1, "flaky": 1, "failed": 1, "not_run": 0,
                          "skipped": 0, "executed": 1, "cached": 2}, summary["tests"])
        self.assertEqual(["//a:t3"], summary["failed_tests"])
        self.assertEqual("TESTS_FAILED (3)", summary["exit"])
        self.assertEqual((12, 30, 4), (summary["actions_executed"], summary["action_cache_hits"],
                                       summary["action_cache_misses"]))
        self.assertEqual({"remote": 10, "remote cache hit": 8}, summary["runners"])
        self.assertEqual(65.4, summary["wall_time_seconds"])


class StagingTest(unittest.TestCase):
    """What the lane stages from a phase's build events, and what it refuses to stage."""

    CFG = "darwin_arm64-fastbuild"

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.dir = Path(directory.name).resolve()
        self.root = self.dir / "output-root"
        self.execroot = self.root / "base" / "execroot" / "_main"
        self.testlogs = self.execroot / "bazel-out" / self.CFG / "testlogs"
        self.out = self.dir / "out"

    def _file(self, path, data=b"failure details"):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    @staticmethod
    def _result(label, *uris, status="FAILED", outputs=None):
        fields = {"testActionOutput": outputs if outputs is not None else
                  [{"name": "test.log", "uri": uri} for uri in uris]}
        if status:
            fields["status"] = status
        return {"id": {"testResult": {"label": label}}, "testResult": fields}

    def _stage(self, *events, budget=lane.MAX_ARTIFACT_TOTAL_BYTES, root=None):
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            files, used = lane.stage_failed_test_outputs(
                _events(*events), self.out, "native", str(root or self.root), budget)
        stage = self.out / lane.ARTIFACTS / "native"
        staged = {path.relative_to(stage).as_posix(): path.read_bytes()
                  for path in stage.rglob("*") if path.is_file()}
        self.assertEqual((len(staged), sum(map(len, staged.values()))), (files, used))
        return staged, printed.getvalue()

    def _assert_refused(self, uri, control=None):
        """Nothing is staged from `uri`, while a valid output after it still is."""
        control = control or self._file(self.testlogs / "ok" / "test.log", b"ok")
        staged, _ = self._stage(self._result("//a:bad", uri, control.as_uri()))
        self.assertEqual({self.CFG + "/ok/test.log": b"ok"}, staged)

    def test_selection_reads_only_this_invocations_results_that_did_not_pass(self):
        logs = "/r/base/execroot/_main/bazel-out/%s/testlogs" % self.CFG
        other = "/r/base/execroot/_main/bazel-out/k8-opt/testlogs"
        lines = _events(
            self._result("//a:passed", "file://%s/a/passed/test.log" % logs, status="PASSED"),
            self._result("//a:failed", outputs=[
                {"uri": "file://%s/a/failed/%s" % (logs, name)} for name in (
                    "test.log", "test.xml", "test.outputs/outputs.zip",
                    # Not collected: the manifest and an earlier attempt's log.
                    "test.outputs_manifest/MANIFEST", "test_attempts/attempt_1.log")]),
            self._result("//a:timeout", "file://%s/a/timeout/test.log" % logs, status="TIMEOUT"),
            # Bazel omits a NO_STATUS result's status.
            self._result("//a:unknown", "file://localhost%s/a/unknown/test.log" % logs,
                         status=None),
            # The same test in a second configuration keeps its own key; a repeat does not.
            self._result("//a:failed", "file://%s/a/failed/test.log" % other,
                         "file://%s/a/failed/test.log" % other),
            {"id": {"testSummary": {"label": "//a:failed"}},
             "testSummary": {"overallStatus": "FAILED"}}) + [
            # A Bazel server stopped mid-write leaves a partial last line.
            '{"id": {"testResult": {"label": "//a:cut']
        selected = [(str(key), path, label) for key, path, label in lane.failed_test_outputs(lines)]
        self.assertEqual([
            (self.CFG + "/a/failed/test.log", logs + "/a/failed/test.log", "//a:failed"),
            (self.CFG + "/a/failed/test.xml", logs + "/a/failed/test.xml", "//a:failed"),
            (self.CFG + "/a/failed/test.outputs/outputs.zip",
             logs + "/a/failed/test.outputs/outputs.zip", "//a:failed"),
            (self.CFG + "/a/timeout/test.log", logs + "/a/timeout/test.log", "//a:timeout"),
            (self.CFG + "/a/unknown/test.log", logs + "/a/unknown/test.log", "//a:unknown"),
            ("k8-opt/a/failed/test.log", other + "/a/failed/test.log", "//a:failed")], selected)
        self.assertEqual(1, lane.summarize(lines)["tests"]["failed"])

    def test_a_downloaded_failed_output_is_staged_and_an_earlier_stage_is_replaced(self):
        log = self._file(self.testlogs / "a" / "failed" / "test.log")
        earlier = self._file(self.out / lane.ARTIFACTS / "native" / "earlier" / "test.log")
        missing = self.testlogs / "a" / "failed" / "test.xml"
        staged, printed = self._stage(self._result("//a:failed", log.as_uri(), missing.as_uri()))
        self.assertEqual({self.CFG + "/a/failed/test.log": b"failure details"}, staged)
        self.assertFalse(earlier.exists())
        self.assertNotIn("::warning::", printed)

    def test_only_a_local_file_uri_is_read(self):
        log = self._file(self.testlogs / "a" / "test.log")
        for uri in ("http://%s" % log, "file://cache.invalid%s" % log,
                    "bytestream://cache.invalid/blobs/0/1"):
            with self.subTest(uri=uri):
                self._assert_refused(uri)

    def test_an_empty_dot_or_dot_dot_segment_is_refused(self):
        # Each names a real file inside the output root once normalized, so only the segment
        # rule refuses it; an empty segment after testlogs would otherwise make a key that
        # starts at the root of the file system.
        self._file(self.testlogs / "etc" / "x" / "test.log")
        for uri in ("file://%s//etc/x/test.log" % self.testlogs,
                    "file://%s/./etc/x/test.log" % self.testlogs,
                    "file://%s/x/../base/execroot/_main/bazel-out/%s/testlogs/etc/x/test.log"
                    % (self.root, self.CFG)):
            with self.subTest(uri=uri):
                self._assert_refused(uri)

    def test_malformed_or_nul_uris_and_odd_fields_are_skipped(self):
        control = self._file(self.testlogs / "ok" / "test.log", b"ok")
        self._file(self.testlogs / "a" / "test.log")
        staged, _ = self._stage(
            self._result("//a:bad", "file://[", "file://%s/a%%00/test.log" % self.testlogs),
            self._result("//a:odd", outputs="file://%s/a/test.log" % self.testlogs),
            self._result("//a:odd", outputs=5),
            self._result("//a:odd", outputs=["x", {"uri": 7}, {"uri": None}]),
            {"id": {"testResult": "x"}, "testResult": {"status": "FAILED"}},
            {"id": {"testResult": {"label": "//a:odd"}}, "testResult": ["x"]},
            self._result("//a:ok", control.as_uri()))
        self.assertEqual({self.CFG + "/ok/test.log": b"ok"}, staged)

    def test_a_symbolic_link_anywhere_below_the_output_root_is_refused(self):
        inside = self._file(self.root / "elsewhere" / "a" / "test.log")
        outside = self._file(self.dir / "outside" / "a" / "test.log")
        self.testlogs.mkdir(parents=True, exist_ok=True)
        (self.testlogs / "inside").symlink_to(inside.parent, target_is_directory=True)
        (self.testlogs / "outside").symlink_to(outside.parent, target_is_directory=True)
        (self.testlogs / "leaf").mkdir()
        (self.testlogs / "leaf" / "test.log").symlink_to(inside)
        for name in ("inside", "outside", "leaf"):
            with self.subTest(link=name):
                self._assert_refused((self.testlogs / name / "test.log").as_uri())

    def test_a_source_outside_the_output_root_is_refused(self):
        elsewhere = self.dir / "other-root" / "base/execroot/_main/bazel-out" / self.CFG
        self._assert_refused(self._file(elsewhere / "testlogs" / "a" / "test.log").as_uri())

    def test_an_output_root_reached_through_a_link_still_stages(self):
        log = self._file(self.testlogs / "a" / "test.log")
        link = self.dir / "root-link"
        link.symlink_to(self.root, target_is_directory=True)
        uri = "file://%s" % (link / log.relative_to(self.root))
        staged, _ = self._stage(self._result("//a:failed", uri), root=link)
        self.assertEqual({self.CFG + "/a/test.log": b"failure details"}, staged)

    def test_only_a_regular_file_is_opened_and_a_final_link_is_not_followed(self):
        regular = self._file(self.dir / "regular", b"12345")
        link = self.dir / "link"
        link.symlink_to(regular)
        fifo = self.dir / "fifo"
        os.mkfifo(fifo)
        opened = lane._open_regular(str(regular))
        self.addCleanup(os.close, opened[0])
        self.assertEqual(5, opened[1])
        hard = self.dir / "hard"
        os.link(self._file(self.dir / "linked", b"x"), hard)
        for path in (link, self.dir, fifo, self.dir / "absent", hard):
            with self.subTest(path=path.name):
                self.assertIsNone(lane._open_regular(str(path)))

    def test_a_hard_link_to_a_file_outside_the_output_root_is_refused(self):
        outside = self._file(self.dir / "outside" / "secret", b"outside sentinel")
        target = self.testlogs / "a" / "test.log"
        target.parent.mkdir(parents=True)
        os.link(outside, target)
        self._assert_refused(target.as_uri())

    def test_a_path_must_name_exactly_one_testlogs_directory(self):
        nested = self.testlogs / "a" / "bazel-out" / "k8-opt" / "testlogs" / "b" / "test.log"
        self._assert_refused(self._file(nested).as_uri())
        self.assertIsNone(lane._staged_key("/r/bazel-out/k/x/test.log"))

    def test_a_symbolic_link_at_the_stage_root_is_refused(self):
        log = self._file(self.testlogs / "a" / "test.log")
        for linked in (self.out / lane.ARTIFACTS, self.out / lane.ARTIFACTS / "native"):
            with self.subTest(link=linked.name):
                outside = self.dir / ("outside-" + linked.name)
                kept = self._file(outside / "native" / "kept", b"kept")
                self._file(outside / "kept", b"kept")
                shutil.rmtree(self.out, ignore_errors=True)
                linked.parent.mkdir(parents=True, exist_ok=True)
                linked.symlink_to(outside, target_is_directory=True)
                with contextlib.redirect_stdout(io.StringIO()) as printed:
                    self.assertEqual((0, 0), lane.stage_failed_test_outputs(
                        _events(self._result("//a:t", log.as_uri())), self.out, "native",
                        str(self.root), lane.MAX_ARTIFACT_TOTAL_BYTES))
                self.assertIn("is a symbolic link; nothing of the native phase is staged",
                              printed.getvalue())
                self.assertEqual([b"kept", b"kept"], sorted(
                    path.read_bytes() for path in outside.rglob("*") if path.is_file()))

    def _phase(self, *events, budget=lane.MAX_ARTIFACT_TOTAL_BYTES, status=3):
        args = lane.parse_args(["--out", str(self.out), "--output-user-root", str(self.root)], {})
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            used = lane._stage_phase(args, _events(*events), "native", budget, status)
        stage = self.out / lane.ARTIFACTS / "native"
        staged = sorted(path.relative_to(stage).as_posix() for path in stage.rglob("*")
                        if path.is_file())
        return used, staged, printed.getvalue()

    def test_a_stage_root_that_cannot_be_cleared_is_a_warning(self):
        stuck = self._file(self.out / lane.ARTIFACTS / "native" / "locked" / "old")
        stuck.parent.chmod(0o500)
        self.addCleanup(stuck.parent.chmod, 0o700)
        log = self._file(self.testlogs / "a" / "test.log")
        used, _, printed = self._phase(self._result("//a:t", log.as_uri()))
        self.assertEqual(0, used)
        self.assertIn("::warning::macOS remote shadow lane: the native phase's failed test "
                      "outputs were not all staged (PermissionError)", printed)

    def test_a_copy_that_raises_leaves_no_partial_file(self):
        from unittest import mock
        log = self._file(self.testlogs / "a" / "test.log")

        def failing_copy(source, destination, limit):
            os.write(destination, b"part")
            raise OSError("disk full")

        with mock.patch.object(lane, "_copy_bounded", side_effect=failing_copy):
            staged, printed = self._stage(self._result("//a:t", log.as_uri()))
        self.assertEqual({}, staged)
        self.assertIn("is not staged (OSError)", printed)

    def test_a_phase_that_stops_part_way_reports_what_it_staged(self):
        from unittest import mock
        first = self._file(self.testlogs / "a" / "test.log", b"x" * 6)
        second = self._file(self.testlogs / "b" / "test.log", b"y" * 6)
        real = lane._copy_bounded
        calls = []

        def copy(source, destination, limit):
            calls.append(limit)
            if len(calls) == 1:
                return real(source, destination, limit)
            os.write(destination, b"part")
            raise RuntimeError("stopped")

        with mock.patch.object(lane, "_copy_bounded", side_effect=copy):
            used, staged, printed = self._phase(self._result("//a:a", first.as_uri()),
                                                self._result("//a:b", second.as_uri()))
        self.assertEqual(6, used)
        self.assertEqual([self.CFG + "/a/test.log"], staged)
        self.assertIn("not all staged (RuntimeError)", printed)
        self.assertIn("staged 1 output files (6 bytes) for native.", printed)

    def test_failed_results_that_stage_nothing_are_a_warning(self):
        outside = self.dir / "elsewhere/base/execroot/_main/bazel-out" / self.CFG / "testlogs"
        log = self._file(outside / "a" / "test.log")
        used, staged, printed = self._phase(self._result("//a:t", log.as_uri()))
        self.assertEqual((0, []), (used, staged))
        self.assertIn("staged 0 output files (0 bytes) for native.", printed)
        self.assertIn("::warning::macOS remote shadow lane: the native phase has 1 test results "
                      "of tests that ran and did not pass but staged no output files", printed)
        _, _, passing = self._phase(self._result("//a:t", log.as_uri(), status="PASSED"),
                                    status=0)
        self.assertNotIn("::warning::", passing)

    def test_tests_that_never_ran_are_not_counted_and_a_build_failure_says_so(self):
        # A build failure: its tests report FAILED_TO_BUILD or no status, and never ran.
        never_ran = [self._result("//a:t%d" % index, status=status)
                     for index, status in enumerate(("FAILED_TO_BUILD", None, "NO_STATUS",
                                                     "TOOL_HALTED_BEFORE_TESTING"))]
        for status in ("FAILED", "TIMEOUT", "INCOMPLETE", "REMOTE_FAILURE"):
            self.assertEqual(1, lane._ran_and_did_not_pass(_events(
                self._result("//a:ran", status=status))))
        self.assertEqual(0, lane._ran_and_did_not_pass(_events(*never_ran)))
        args = lane.parse_args(["--out", str(self.out), "--output-user-root", str(self.root)], {})
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            lane._stage_phase(args, _events(*never_ran), "native",
                              lane.MAX_ARTIFACT_TOTAL_BYTES, 1, build_failures=60)
        self.assertNotIn("staged no output files", printed.getvalue())
        self.assertIn("the native phase failed to build 60 targets, so their tests did not run; "
                      "the failing actions' output is above in this log",
                      printed.getvalue())

    def test_the_run_cap_carries_across_phases(self):
        from unittest import mock
        logs = {phase: self._file(self.testlogs / phase / "test.log", b"x" * 6)
                for phase in lane.PHASES}

        def fake_phase(command, environment, console, **_):
            phase = "gpu" if "--config=remote-macos-gpu" in command else "native"
            events = next(word.split("=", 1)[1] for word in command
                          if word.startswith("--build_event_json_file="))
            Path(events).write_text(
                "\n".join(_events(self._result("//a:" + phase, logs[phase].as_uri()))) + "\n",
                encoding="utf-8")
            return 3, None

        args = lane.parse_args(["--out", str(self.out), "--fallback", "true",
                                "--output-user-root", str(self.root)], {})
        with mock.patch.object(lane, "_preflight", return_value=(None, 0)), \
                mock.patch.object(lane, "run_phase", side_effect=fake_phase), \
                mock.patch.object(lane, "MAX_ARTIFACT_TOTAL_BYTES", 10), \
                contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(3, lane.run(args, {}))
        self.assertTrue((self.out / lane.ARTIFACTS / "native" / self.CFG / "native" /
                         "test.log").is_file())
        self.assertFalse((self.out / lane.ARTIFACTS / "gpu").exists() and any(
            path.is_file() for path in (self.out / lane.ARTIFACTS / "gpu").rglob("*")))
        self.assertIn("gpu/test.log of //a:gpu is not uploaded: 6 bytes is over the per-run cap "
                      "of 4 bytes", printed.getvalue())

    def test_a_staged_file_stays_inside_the_stage_root(self):
        stage = self.out / "stage"
        outside = self.dir / "outside-stage"
        outside.mkdir()
        stage.mkdir(parents=True)
        (stage / "link").symlink_to(outside, target_is_directory=True)
        self.assertEqual(stage / "cfg" / "a" / "test.log",
                         lane._destination(stage, "cfg/a/test.log"))
        # "a/../b" stays inside, so only the lexical rule refuses it; "link/x" passes that rule,
        # so only the resolved containment check refuses it.
        for key in ("/etc/x", "a/../b", "a/../../x", "link/x"):
            with self.subTest(key=key):
                with self.assertRaises(ValueError):
                    lane._destination(stage, key)
        self.assertEqual([], list(outside.iterdir()))

    def test_a_file_over_the_per_file_cap_is_skipped_and_named(self):
        from unittest import mock
        big = self._file(self.testlogs / "big" / "test.log", b"x" * 11)
        small = self._file(self.testlogs / "small" / "test.log", b"x" * 10)
        with mock.patch.object(lane, "MAX_ARTIFACT_FILE_BYTES", 10):
            staged, printed = self._stage(self._result("//a:big", big.as_uri()),
                                          self._result("//a:small", small.as_uri()))
        self.assertEqual([self.CFG + "/small/test.log"], list(staged))
        self.assertIn("::warning::", printed)
        self.assertIn("big/test.log of //a:big is not uploaded: 11 bytes is over the per-file "
                      "cap of 10 bytes", printed)

    def test_files_over_the_per_run_cap_are_skipped_and_named(self):
        first = self._file(self.testlogs / "first" / "test.log", b"x" * 6)
        second = self._file(self.testlogs / "second" / "test.log", b"x" * 6)
        staged, printed = self._stage(self._result("//a:first", first.as_uri()),
                                      self._result("//a:second", second.as_uri()), budget=10)
        self.assertEqual([self.CFG + "/first/test.log"], list(staged))
        self.assertIn("second/test.log of //a:second is not uploaded: 6 bytes is over the "
                      "per-run cap of 4 bytes", printed)

    def test_the_copy_itself_is_bounded(self):
        # A file that grows after it was measured: the copy stops at the cap plus one byte.
        source = self._file(self.dir / "growing", b"x" * 20)
        for limit, expected in ((20, 20), (19, None), (25, 20)):
            with self.subTest(limit=limit):
                target = self.dir / ("copy-%d" % limit)
                read = os.open(source, os.O_RDONLY)
                write = os.open(target, os.O_WRONLY | os.O_CREAT)
                try:
                    self.assertEqual(expected, lane._copy_bounded(read, write, limit))
                finally:
                    os.close(read)
                    os.close(write)
                self.assertLessEqual(target.stat().st_size, limit)

    def test_staging_and_summary_failures_never_cost_the_report(self):
        from unittest import mock
        log = self._file(self.testlogs / "a" / "test.log")
        bep = _events({"id": {"testSummary": {"label": "//a:t"}},
                       "testSummary": {"overallStatus": "FAILED"}},
                      self._result("//a:t", "file://[", "file://%s/a%%00/test.log" % self.testlogs,
                                   log.as_uri()),
                      self._result("//a:t", outputs="not a list"))

        def fake_phase(command, environment, console, **_):
            events = next(word.split("=", 1)[1] for word in command
                          if word.startswith("--build_event_json_file="))
            Path(events).write_text("\n".join(bep) + "\n", encoding="utf-8")
            return 3, None

        for name, patch in (
                ("malformed events", None),
                ("staging raises", mock.patch.object(lane, "stage_failed_test_outputs",
                                                     side_effect=RuntimeError("x"))),
                ("summary raises", mock.patch.object(lane, "summarize",
                                                     side_effect=self._raising_summarize()))):
            with self.subTest(case=name):
                out = self.dir / ("out-" + name.replace(" ", "-"))
                args = lane.parse_args(["--out", str(out), "--fallback", "true",
                                        "--output-user-root", str(self.root)], {})
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(lane, "_preflight",
                                                          return_value=(None, 0)))
                    stack.enter_context(mock.patch.object(lane, "run_phase",
                                                          side_effect=fake_phase))
                    if patch is not None:
                        stack.enter_context(patch)
                    printed = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    status = lane.run(args, {})
                self.assertEqual(3, status)
                self.assertTrue((out / "report.md").is_file())
                summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
                self.assertEqual(3, summary["phases"]["native"]["status"])
                if name == "malformed events":
                    self.assertEqual(
                        [self.CFG + "/a/test.log"],
                        [path.relative_to(out / lane.ARTIFACTS / "native").as_posix()
                         for path in (out / lane.ARTIFACTS / "native").rglob("*")
                         if path.is_file()])
                else:
                    self.assertIn("::warning::", printed.getvalue())

    @staticmethod
    def _raising_summarize():
        real = lane.summarize

        def summarize(lines):
            if lines:
                raise TypeError("malformed")
            return real(lines)
        return summarize

    def test_the_collector_reads_bazel_testlogs_when_its_input_is_empty(self):
        workspace = self.dir / "workspace"
        workspace.mkdir()
        logs = self.dir / "logs"
        self._file(logs / "pkg" / "test" / "test.log", b"from bazel-testlogs")
        self._file(logs / "pkg" / "test" / "test.outputs" / "actual.png", b"image")
        (workspace / "bazel-testlogs").symlink_to(logs, target_is_directory=True)
        self.assertEqual({"pkg/test/test.log": "from bazel-testlogs",
                          "pkg/test/test.outputs/actual.png": "image"},
                         _collect(self, "", cwd=str(workspace)))


if __name__ == "__main__":
    unittest.main()
