"""Runs CI lane phases as a workflow step does, against recording stand-ins for their tools.

Each case runs `python3 tools/ci/lane.py phase <lane> <phase>` in a fixture checkout whose
bazelisk, jq and helper scripts record their calls and answer from the case's configuration,
then checks the commands, their order, the messages and the exit status: what the step scripts
these phases replaced did under `set -euo pipefail`. The commands each phase runs are pinned
separately, against the workflow's scenarios, by //tools:ci_lane_parity_tests.
"""

import ast
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory, write_python_stand_in

_SOURCES = ("tools/ci/lane.py", "tools/ci/lanes.py", "tools/ci_bazel_test.sh")

# One stand-in for every tool. It records its name and arguments, then answers from the JSON
# configuration in STUB_CONFIG: a list of answers per tool (or per `bazelisk <command>`), used
# in order, the last one repeating. An answer is [stdout, status] or [stdout, status, stderr], with
# status "signal" to end by SIGTERM.
_RECORDER = r'''
import json, os, signal, sys
from pathlib import Path

tool, args = sys.argv[1], sys.argv[2:]
if tool == "watchdog":
    with open(os.environ["CAPTURE"], "a", encoding="utf-8") as capture:
        capture.write(json.dumps(["watchdog", args[0]]) + "\n")
    os.execvp(args[1], args[1:])
with open(os.environ["CAPTURE"], "a", encoding="utf-8") as capture:
    capture.write(json.dumps([tool, *args]) + "\n")
config = json.loads(Path(os.environ["STUB_CONFIG"]).read_text(encoding="utf-8"))
key = tool
if tool == "bazelisk":
    key = "bazelisk " + next(arg for arg in args if not arg.startswith("--"))
answers = config.get(key, [["", 0]])
counts_path = Path(os.environ["CAPTURE"] + ".counts")
counts = json.loads(counts_path.read_text()) if counts_path.exists() else {}
index = counts.get(key, 0)
counts[key] = index + 1
counts_path.write_text(json.dumps(counts))
output, status, *error = answers[min(index, len(answers) - 1)]
sys.stdout.write(output)
sys.stdout.flush()
sys.stderr.write("".join(error))
sys.stderr.flush()
if status == "signal":
    os.kill(os.getpid(), signal.SIGTERM)
sys.exit(status)
'''


class LaneTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.resolve = runfiles.Create().Rlocation
        cls.stubs = ExecutableFixtureDirectory()
        cls.stubs.write_python3()
        recorder = cls.stubs.write("recorder.py", _RECORDER)
        for tool in ("bazelisk", "jq", "df", "du"):
            write_python_stand_in(cls.stubs.path / tool, recorder, tool)
        cls.recorder = recorder

    @classmethod
    def tearDownClass(cls):
        cls.stubs.cleanup()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lane ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "checkout"
        for source in _SOURCES:
            target = self.checkout / source
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(self.resolve("donner/" + source), target)
        write_python_stand_in(self.checkout / "tools/ci/bazel_watchdog.sh", self.recorder,
                              "watchdog")
        # Python helpers the phases run as `python3 <script>`.
        for name, tool in (("tools/ci_remote_gpu_targets.py", "remote_gpu"),
                           ("tools/browser_gpu_test_summary.py", "summary")):
            (self.checkout / name).write_text(
                "import os, sys\n"
                "os.execv(sys.executable, [sys.executable, %r, %r] + sys.argv[1:])\n"
                % (str(self.recorder), tool), encoding="utf-8")
        self.capture = self.root / "capture.jsonl"
        self.config = self.root / "config.json"
        self.runner_temp = self.root / "runner-temp"
        self.runner_temp.mkdir()

    def run_phase(self, lane, phase, config=None, *, dry_run=False, **environment):
        self.config.write_text(json.dumps(config or {}), encoding="utf-8")
        env = {"PATH": "%s:%s" % (self.stubs.path, os.environ["PATH"]),
               "CAPTURE": str(self.capture), "STUB_CONFIG": str(self.config),
               "RUNNER_TEMP": str(self.runner_temp), "DIAG_DIR": "/diag",
               "GITHUB_WORKSPACE": str(self.checkout)}
        env.update(environment)
        env = {key: value for key, value in env.items() if value is not None}
        argv = [str(self.stubs.path / "python3"), "tools/ci/lane.py", "phase", lane, phase]
        if dry_run:
            argv.append("--dry-run")
        result = subprocess.run(argv, cwd=self.checkout, env=env, capture_output=True,
                                text=True, timeout=test_timing.STALL_GUARD_SECONDS,
                                check=False)
        calls = []
        if self.capture.exists():
            calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        return result, calls

    def reset(self):
        for path in (self.capture, Path(str(self.capture) + ".counts")):
            if path.exists():
                path.unlink()

    def bazel_calls(self, calls):
        return [call for call in calls if call[0] == "bazelisk"]

    # --- Selection. ------------------------------------------------------------------------------

    def test_selection_is_the_affected_labels_or_everything_on_a_fallback(self):
        result, calls = self.run_phase("macos", "build", TARGETS_FALLBACK="false",
                                       TARGETS_AFFECTED="//a:a  //b:b\n//c:c\n",
                                       BAZEL_MACOS_BUILD_FLAGS=" --xcode_version=26.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bazel_calls(calls), [[
            "bazelisk", "build", "--config=ci", "--config=ci-macos", "--macos_minimum_os=13.3",
            "--xcode_version=26.1", "//a:a", "//b:b", "//c:c"]])
        self.reset()
        result, calls = self.run_phase("macos", "build", TARGETS_FALLBACK="true",
                                       TARGETS_AFFECTED="//a:a")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bazel_calls(calls)[0][-1], "//...")

    def test_buildbuddy_flags_split_into_words(self):
        result, calls = self.run_phase("linux", "build", TARGETS_FALLBACK="false",
                                       TARGETS_AFFECTED="//a:a",
                                       BUILDBUDDY_FLAGS="--config=buildbuddy-re  --x=1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bazel_calls(calls), [[
            "bazelisk", "build", "--config=ci", "--config=latest_llvm",
            "--config=buildbuddy-re", "--x=1", "//a:a"]])

    def test_empty_selection_fails_a_guarded_phase_before_bazel(self):
        for lane, phase, where in (("linux", "test", "the test step"),
                                   ("linux-self-hosted", "local_xvfb",
                                    "the local Xvfb test step"),
                                   ("macos-self-hosted", "browser_test",
                                    "the browser test step"),
                                   ("macos", "browser_preflight", "the browser preflight")):
            with self.subTest(lane=lane, phase=phase):
                result, calls = self.run_phase(lane, phase, TARGETS_FALLBACK="false",
                                               TARGETS_AFFECTED=" \n\t")
                self.assertEqual(result.returncode, 1)
                self.assertIn("::error::Empty target selection reached %s." % where,
                              result.stdout)
                self.assertEqual(self.bazel_calls(calls), [])

    def test_empty_selection_guard_rejects_only_whitespace(self):
        # The step scripts derived the selection with `$(echo "<affected>")` and rejected it with
        # `[[ "$TARGETS" =~ ^[[:space:]]*$ ]]`; the phase decides every case the same way.
        script = ('TARGETS=$(echo "$1")\n'
                  'if [[ "$TARGETS" =~ ^[[:space:]]*$ ]]; then echo empty; else echo nonempty; fi')
        cases = {"": "empty", "   ": "empty", " \t ": "empty", "\n\n": "empty",
                 " \n ": "empty", "\r": "empty", "//...": "nonempty",
                 "  //a:b  //c:d  ": "nonempty", "//a:b\n//c:d": "nonempty",
                 "\n//a:b\n": "nonempty"}
        for value, expected in cases.items():
            with self.subTest(value=value):
                shell = subprocess.run(
                    ["bash", "--noprofile", "--norc", "-euo", "pipefail", "-c", script,
                     "guard", value], capture_output=True, text=True, check=True)
                self.assertEqual(shell.stdout.strip(), expected)
                result, _ = self.run_phase("linux", "test", TARGETS_FALLBACK="false",
                                           TARGETS_AFFECTED=value)
                self.assertEqual(result.returncode == 1 and "Empty target selection" in
                                 result.stdout, expected == "empty", result.stdout)
                self.reset()

    def test_local_xvfb_tolerates_a_selection_without_display_tests(self):
        native = "//donner/editor/tests:editor_window_vulkan_surface_tests"
        for fallback, affected, target, status in (
                ("false", native, native, 0),
                ("false", "//donner/base:base_tests", "//donner/base:base_tests", 4),
                ("true", "", "//...", 0)):
            with self.subTest(fallback=fallback, affected=affected):
                result, calls = self.run_phase("linux-self-hosted", "local_xvfb",
                                               {"bazelisk test": [["", status]]},
                                               TARGETS_FALLBACK=fallback,
                                               TARGETS_AFFECTED=affected)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                [call] = self.bazel_calls(calls)
                self.assertIn("--test_tag_filters=requires_xvfb,-fuzz_soak", call)
                self.assertEqual(call[-1], target)
                self.assertEqual("No test targets" in result.stdout, status == 4)
                self.reset()

    def test_an_unguarded_build_runs_with_an_empty_selection(self):
        result, calls = self.run_phase("linux", "build", TARGETS_FALLBACK="false",
                                       TARGETS_AFFECTED="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bazel_calls(calls),
                         [["bazelisk", "build", "--config=ci", "--config=latest_llvm"]])

    def test_a_missing_selection_fails_instead_of_selecting_nothing(self):
        result, calls = self.run_phase("linux", "build")
        self.assertEqual(result.returncode, 1)
        self.assertIn("TARGETS_FALLBACK and TARGETS_AFFECTED are unset", result.stderr)
        self.assertEqual(calls, [])

    # --- Exit status. ----------------------------------------------------------------------------

    def test_bazel_status_is_the_phase_status(self):
        for status in (1, 3, 37):
            with self.subTest(status=status):
                result, _ = self.run_phase("linux", "build", {"bazelisk build": [["", status]]},
                                           TARGETS_FALLBACK="true")
                self.assertEqual(result.returncode, status)

    def test_the_test_wrapper_tolerates_only_an_empty_test_set(self):
        for status, expected in ((4, 0), (3, 3), (0, 0)):
            with self.subTest(status=status):
                result, _ = self.run_phase("linux", "test", {"bazelisk test": [["", status]]},
                                           TARGETS_FALLBACK="true")
                self.assertEqual(result.returncode, expected)
                self.assertEqual("No test targets in the selected target set" in result.stdout,
                                 status == 4)

    def test_a_command_killed_by_a_signal_reports_128_plus_the_signal(self):
        result, _ = self.run_phase("linux", "build", {"bazelisk build": [["", "signal"]]},
                                   TARGETS_FALLBACK="true")
        self.assertEqual(result.returncode, 128 + signal.SIGTERM)

    def test_a_missing_command_reports_127(self):
        result, _ = self.run_phase("linux", "build", TARGETS_FALLBACK="true",
                                   PATH=str(self.root / "empty"))
        self.assertEqual(result.returncode, 127)

    def test_the_first_failing_command_ends_the_phase(self):
        result, calls = self.run_phase("linux", "buildbuddy_verify", {"jq": [["", 1]]},
                                       BUILDBUDDY_FLAGS="--config=buildbuddy-re",
                                       GITHUB_RUN_ID="7", GITHUB_RUN_ATTEMPT="2")
        self.assertEqual(result.returncode, 1)
        self.assertEqual([call[0] for call in calls], ["bazelisk", "jq"])
        self.assertIn("--copt=-DBUILDBUDDY_RE_PROBE_7_2", calls[0])
        self.assertEqual(calls[1][-1], "%s/buildbuddy-execution.json" % self.runner_temp)

    def test_an_unset_runner_variable_fails_like_an_unbound_variable(self):
        result, calls = self.run_phase("linux-self-hosted", "build", TARGETS_FALLBACK="true",
                                       DIAG_DIR=None)
        self.assertEqual(result.returncode, 1)
        self.assertIn("DIAG_DIR: unbound variable", result.stderr)
        self.assertEqual(calls, [])

    def test_advisory_phase_reports_the_first_deferred_failure(self):
        for bazel_status, summary_status, expected in ((3, 0, 3), (0, 2, 2), (3, 2, 3),
                                                       (0, 0, 0)):
            with self.subTest(bazel=bazel_status, summary=summary_status):
                result, calls = self.run_phase("macos", "browser_gpu_serial", {
                    "bazelisk test": [["", bazel_status]], "summary": [["", summary_status]]})
                self.assertEqual(result.returncode, expected)
                self.assertEqual([call[0] for call in calls], ["bazelisk", "summary"])
                self.reset()

    # --- Phase-specific actions. -----------------------------------------------------------------

    def test_browser_preflight_runs_only_when_a_browser_test_is_selected(self):
        result, calls = self.run_phase("macos-self-hosted", "browser_preflight",
                                       {"bazelisk query": [["\n", 0]]},
                                       TARGETS_FALLBACK="false", TARGETS_AFFECTED="//a:a //b:b")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No browser targets selected.", result.stdout)
        self.assertEqual(self.bazel_calls(calls), [[
            "bazelisk", "--nohome_rc", "--nosystem_rc", "query",
            "attr('tags', 'launches-browser', tests(set(//a:a //b:b)))"]])
        # Each command is echoed as a shell reads it back: the query expression stays one word.
        self.assertIn("+ bazelisk --nohome_rc --nosystem_rc query "
                      "'attr('\"'\"'tags'\"'\"', '\"'\"'launches-browser'\"'\"', "
                      "tests(set(//a:a //b:b)))'\n", result.stdout)
        self.reset()
        result, calls = self.run_phase("macos-self-hosted", "browser_preflight",
                                       {"bazelisk query": [["//a:a\n", 0]]},
                                       TARGETS_FALLBACK="false", TARGETS_AFFECTED="//a:a")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bazel_calls(calls)[1][-1], "//tools/ci:mac_browser_archive")
        self.reset()
        result, calls = self.run_phase("macos-self-hosted", "browser_preflight",
                                       {"bazelisk query": [["", 7]]}, TARGETS_FALLBACK="true")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(len(self.bazel_calls(calls)), 1)

    def test_self_hosted_linux_tests_the_remote_gpu_mapping(self):
        result, calls = self.run_phase("linux-self-hosted", "test",
                                       {"remote_gpu": [["//a:a_remote\n//b:b\n", 0]]},
                                       TARGETS_FALLBACK="false", TARGETS_AFFECTED="//a:a //b:b")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], ["remote_gpu", "//a:a", "//b:b"])
        self.assertEqual(calls[1], ["watchdog", "/diag/test/console.log"])
        self.assertEqual(calls[2][-2:], ["//a:a_remote", "//b:b"])
        for answer, message in ((["//a:a", 1], "Failed to select remote GPU test targets."),
                                ([" \n", 0], "Remote GPU target selection became empty.")):
            with self.subTest(message=message):
                self.reset()
                result, calls = self.run_phase("linux-self-hosted", "test",
                                               {"remote_gpu": [answer]},
                                               TARGETS_FALLBACK="true")
                self.assertEqual(result.returncode, 1)
                self.assertIn("::error::" + message, result.stdout)
                self.assertEqual(self.bazel_calls(calls), [])

    def test_failed_toolchain_fetch_resets_the_partial_repository_and_retries_once(self):
        output_base = self.root / "output-base"
        partial = output_base / "external" / "rules+llvm_toolchain_linux_aarch64_llvm"
        partial.mkdir(parents=True)
        kept = output_base / "external" / "other"
        kept.mkdir()
        config = {"bazelisk fetch": [["", 1], ["", 0]],
                  "bazelisk info": [[str(output_base) + "\n", 0, "info-stderr\n"]],
                  "bazelisk shutdown": [["", 3, "shutdown-stderr\n"]],
                  "df": [["df-stdout\n", 1, "df-stderr\n"]],
                  "du": [["du-stdout\n", 1, "du-stderr\n"]]}
        result, calls = self.run_phase("linux-self-hosted", "fetch", config,
                                       TARGETS_FALLBACK="true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[1] for call in self.bazel_calls(calls)],
                         ["fetch", "info", "shutdown", "fetch"])
        # Standard error is discarded only where the step script discarded it: `info` and `du`
        # (and `find`); `shutdown` and `df` failures are ignored but still report.
        for shown in ("shutdown-stderr", "df-stderr"):
            self.assertIn(shown, result.stderr)
        for hidden in ("info-stderr", "du-stderr"):
            self.assertNotIn(hidden, result.stderr)
        self.assertIn("df-stdout", result.stdout)
        self.assertIn("du-stdout", result.stdout)
        self.assertEqual(["df", "-h", "/var/cache/bazel", str(self.checkout)],
                         next(call for call in calls if call[0] == "df"))
        self.assertEqual(["du", "-sh", str(partial)],
                         next(call for call in calls if call[0] == "du"))
        self.assertFalse(partial.exists())
        self.assertTrue(kept.exists())
        self.assertIn("::group::Bazel disk state before LLVM fetch retry", result.stdout)
        self.reset()
        config["bazelisk fetch"] = [["", 5], ["", 6]]
        result, calls = self.run_phase("linux-self-hosted", "fetch", config,
                                       TARGETS_FALLBACK="true")
        self.assertEqual(result.returncode, 6)

    def test_vm_qualification_requires_exactly_four_executed_tests(self):
        receipt = self.runner_temp / "coverage-receipt" / "coverage-receipt.json"
        receipt.parent.mkdir()
        for executed, tests, expected in ((4, 4, 0), (3, 4, 1), (4, 5, 1)):
            with self.subTest(executed=executed, tests=tests):
                receipt.write_text(json.dumps({"steps": {"qualification": {
                    "executed": executed, "tests": tests}}}))
                result, _ = self.run_phase("macos-vm-qualification", "require_executed")
                self.assertEqual(result.returncode, expected)
                self.assertEqual("Expected four executed qualification tests" in result.stderr,
                                 bool(expected))

    def test_hosted_macos_test_reports_its_exclusions(self):
        result, _ = self.run_phase("macos", "test", TARGETS_FALLBACK="true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::notice::Excluding requires_metal_toolchain targets", result.stdout)
        self.assertIn("::notice::Excluding requires_shader_validation targets", result.stdout)

    # --- Dry run and interface. ------------------------------------------------------------------

    def test_dry_run_prints_commands_and_runs_nothing(self):
        result, calls = self.run_phase("linux-self-hosted", "build", dry_run=True,
                                       DIAG_DIR=None, TARGETS_FALLBACK="false",
                                       TARGETS_AFFECTED="//a:a //b:b")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])
        description = json.loads(result.stdout)
        self.assertEqual(description["env"], {})
        self.assertEqual(description["actions"][0]["argv"][:3],
                         ["tools/ci/bazel_watchdog.sh", "$DIAG_DIR/build/console.log",
                          "bazelisk"])
        self.assertEqual(description["actions"][0]["argv"][-2:], ["//a:a", "//b:b"])

    def test_dry_run_without_a_selection_prints_its_symbol(self):
        result, _ = self.run_phase("linux-self-hosted", "test", dry_run=True)
        actions = json.loads(result.stdout)["actions"]
        self.assertEqual(actions[-1]["argv"][-1],
                         "$(python3 tools/ci_remote_gpu_targets.py @selected_targets)")

    def test_unknown_phase_is_a_usage_error(self):
        result, calls = self.run_phase("linux", "nonexistent")
        self.assertEqual(result.returncode, 2)
        self.assertIn("phases: buildbuddy_verify", result.stderr)
        self.assertEqual(calls, [])

    def test_sources_parse_as_the_oldest_runner_python(self):
        # Self-hosted macOS jobs without the Homebrew PATH run the system python3 (3.9).
        for source in ("tools/ci/lane.py", "tools/ci/lanes.py"):
            with self.subTest(source=source):
                ast.parse(Path(self.resolve("donner/" + source)).read_text(encoding="utf-8"),
                          feature_version=(3, 9))


if __name__ == "__main__":
    unittest.main()
