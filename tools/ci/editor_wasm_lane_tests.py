"""Runs the Editor WASM lane script against recording stand-ins for Bazel and its checks."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory, write_python_stand_in

_GEODE_MODULE_TARGETS = ["//tools/ci:browser_bridge_link_probes",
                         "//tools/ci:geode_wasm_browser_module"]
# Each subcommand's exact commands, in the order the workflow step ran them. A changed flag,
# configuration, target or order is a change to what CI runs and must update this table.
_COMMANDS = {
    "prefetch": [
        ["bazelisk", "fetch", "//tools/ci:editor_wasm_audits"],
        ["bazelisk", "fetch", "--config=wasm-size", "//tools/ci:wasm_size_tests"],
        ["bazelisk", "fetch", "--config=wasm-size", "--//donner/svg/renderer:text=true",
         "//tools/ci:wasm_text_resource_tests"],
        ["bazelisk", "fetch", "--config=editor-wasm", "//tools/ci:editor_wasm_size_tests"],
        ["bazelisk", "fetch", "--config=wasm-geode", *_GEODE_MODULE_TARGETS],
    ],
    "build": [
        ["bazelisk", "test", "--test_output=errors", "//tools/ci:editor_wasm_audits"],
        ["bazelisk", "test", "--config=editor-wasm", "--test_output=errors",
         "//tools/ci:editor_wasm_shipped_browser_audit"],
        ["bazelisk", "test", "--config=wasm-size", "--test_output=errors",
         "//donner/svg/renderer/wasm:render_test"],
        ["bazelisk", "test", "--config=wasm-size", "--//donner/svg/renderer:text=true",
         "--test_output=errors", "//tools/ci:wasm_text_resource_tests"],
        ["bazelisk", "test", "--config=wasm-size", "--test_output=errors",
         "--test_arg=--payload-budget-mode=enforce", "//donner/svg/renderer/wasm:wasm_size_tests"],
        ["bazelisk", "test", "--config=editor-wasm", "--test_output=errors",
         "--remote_download_outputs=all", "--test_arg=--payload-budget-mode=enforce",
         "//tools/ci:editor_wasm_size_tests"],
    ],
    "prove-lockfile": [
        ["bazelisk", "build", "--config=editor-wasm", "--lockfile_mode=update",
         "//tools/ci:editor_wasm_package"],
        ["bazelisk", "build", "--config=editor-wasm", "--lockfile_mode=error",
         "//tools/ci:editor_wasm_package"],
        ["python3", "-B", "-m", "tools.release_cli", "check-lockfile",
         "--lockfile", "MODULE.bazel.lock"],
        ["cp", "MODULE.bazel.lock", "SNAPSHOT"],
    ],
    "link-bridge": [
        ["bazelisk", "build", "--config=wasm-geode", *_GEODE_MODULE_TARGETS],
        ["bazelisk", "test", "--config=wasm-geode", "--test_output=errors",
         "//tools/ci:geode_wasm_browser_default_audit"],
    ],
}


class EditorWasmLaneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.scripts = [Path(resolver.Rlocation("donner/tools/ci/" + name))
                       for name in ("editor_wasm_lane.sh", "lane.sh")]
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.path / "record.py"
        # Records the command and its working directory, exits with FAIL's status when this is
        # call number FAIL_CALL, and otherwise performs a recorded `cp`.
        recorder.write_text(
            "import json, os, shutil, sys\n"
            "command = sys.argv[1:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([command, os.getcwd()]) + '\\n')\n"
            "with open(os.environ['CAPTURE'], encoding='utf-8') as capture:\n"
            "    call = len(capture.read().splitlines())\n"
            "if str(call) == os.environ['FAIL_CALL']:\n"
            "    sys.exit(int(os.environ['FAIL_STATUS']))\n"
            "if command[0] == 'cp':\n"
            "    shutil.copyfile(command[1], command[2])\n",
            encoding="utf-8",
        )
        (cls.executables.path / "bin").mkdir()
        for tool in ("bazelisk", "python3", "cp"):
            write_python_stand_in(cls.executables.path / "bin" / tool, recorder, tool)
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in ("dirname", "sed"):
            os.symlink(shutil.which(tool), cls.host / tool)
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="editor wasm lane ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "ci").mkdir(parents=True)
        for script in self.scripts:
            shutil.copy(script, self.repo / "tools" / "ci" / script.name)
        (self.repo / "MODULE.bazel.lock").write_text('{"lockFileVersion": 26}\n')
        self.snapshot = self.work / "runner temp" / "editor-wasm-module.lock"
        self.snapshot.parent.mkdir()
        self.capture = self.work / "calls.jsonl"
        self.summary = self.work / "summary.md"

    def _expected(self, command):
        return [[str(self.snapshot) if word == "SNAPSHOT" else word for word in call]
                for call in _COMMANDS[command]]

    def _run(self, *args, fail_call=0, fail_status=0, budget_mode=None, source_revision=None):
        self.capture.write_text("", encoding="utf-8")
        self.summary.write_text("", encoding="utf-8")
        env = dict(os.environ, CAPTURE=str(self.capture), FAIL_CALL=str(fail_call),
                   FAIL_STATUS=str(fail_status), GITHUB_STEP_SUMMARY=str(self.summary),
                   PATH=os.pathsep.join((str(self.executables.path / "bin"), str(self.host))))
        env.pop("GITHUB_SHA", None)
        if source_revision is not None:
            env["GITHUB_SHA"] = source_revision
        env.pop("PAYLOAD_BUDGET_MODE", None)
        if budget_mode is not None:
            env["PAYLOAD_BUDGET_MODE"] = budget_mode
        result = subprocess.run([self.bash, str(self.repo / "tools/ci/editor_wasm_lane.sh"),
                                 *args], cwd=self.work, env=env, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = []
        for line in self.capture.read_text().splitlines():
            command, cwd = json.loads(line)
            self.assertEqual(cwd, str(self.repo.resolve()), command)
            calls.append(command)
        return result, calls

    def _args(self, command):
        return (command, str(self.snapshot)) if command == "prove-lockfile" else (command,)

    def test_each_subcommand_runs_the_workflow_commands_in_order_from_the_repository_root(self):
        for command in _COMMANDS:
            with self.subTest(command=command):
                result, calls = self._run(*self._args(command))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(calls, self._expected(command))
                count = len(_COMMANDS[command])
                self.assertEqual(result.stdout.count("::group::"), count)
                self.assertEqual(result.stdout.count("::endgroup::"), count)
                self.assertEqual(self.summary.read_text().count("| passed |"), count)

    def test_ci_source_revision_reaches_every_editor_package_phase(self):
        for revision in ("a" * 40, "b" * 40):
            for command in ("prefetch", "build", "prove-lockfile"):
                result, calls = self._run(*self._args(command), source_revision=revision)
                self.assertEqual(result.returncode, 0, result.stderr)
                editor_calls = [call for call in calls if "--config=editor-wasm" in call]
                self.assertGreater(len(editor_calls), 0, command)
                for call in editor_calls:
                    flags = [word for word in call if word.startswith("--//build_defs:release_source_commit=")]
                    self.assertEqual(flags, ["--//build_defs:release_source_commit=" + revision], call)

    def test_invalid_ci_source_revision_is_refused_before_running_commands(self):
        for revision in ("", "short", "A" * 40, "a" * 39, "a" * 41):
            result, calls = self._run("build", source_revision=revision)
            self.assertNotEqual(result.returncode, 0, revision)
            self.assertEqual(calls, [], revision)

    def test_main_report_mode_only_changes_the_two_numeric_budget_commands(self):
        result, calls = self._run("build", budget_mode="report")
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = self._expected("build")
        for call in expected:
            if "--test_arg=--payload-budget-mode=enforce" in call:
                call[call.index("--test_arg=--payload-budget-mode=enforce")] = (
                    "--test_arg=--payload-budget-mode=report")
                call[call.index("--test_output=errors")] = "--test_output=all"
        self.assertEqual(calls, expected)
        budget_calls = [call for call in calls if "--test_arg=--payload-budget-mode=report" in call]
        self.assertEqual(len(budget_calls), 2)
        self.assertTrue(any("//donner/svg/renderer/wasm:wasm_size_tests" in call
                            for call in budget_calls))
        self.assertTrue(any("//tools/ci:editor_wasm_size_tests" in call for call in budget_calls))
        pixel_calls = [call for call in calls if "//donner/svg/renderer/wasm:render_test" in call]
        self.assertEqual(len(pixel_calls), 1)
        self.assertFalse(any(word.startswith("--test_arg") for word in pixel_calls[0]))

    def test_explicit_enforcement_matches_local_pr_nightly_and_manual_defaults(self):
        default, default_calls = self._run("build")
        enforced, enforced_calls = self._run("build", budget_mode="enforce")
        self.assertEqual(default.returncode, 0, default.stderr)
        self.assertEqual(enforced.returncode, 0, enforced.stderr)
        self.assertEqual(enforced_calls, default_calls)
        self.assertFalse(any("report" in word for call in default_calls for word in call))

    def test_invalid_budget_mode_is_rejected_before_running_commands(self):
        result, calls = self._run("build", budget_mode="disabled")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(calls, [])

    def test_report_mode_reports_all_numeric_overages_but_keeps_artifact_contracts(self):
        resolver = runfiles.Create()
        script = resolver.Rlocation("donner/donner/editor/wasm/wasm_package_size_tests.py")
        package = self.work / "package"
        package.mkdir()
        (package / "editor.wasm").write_bytes(
            b"\x00asm\x01\x00\x00\x00\x0a\x04\x01\x02\x00\x0b\x0b\x03\x01\x01\x00")
        (package / "editor.js").write_text("requiredBridge")
        command = [sys.executable, script, "--package-dir", str(package),
                   "--max-wasm-raw-bytes=1", "--max-wasm-gzip-bytes=1",
                   "--max-js-raw-bytes=1", "--max-js-gzip-bytes=1",
                   "--max-total-raw-bytes=1", "--max-wasm-function-body-bytes=1",
                   "--max-wasm-data-segments=0", "--expected-js-property=requiredBridge"]
        for mode, expected in (("enforce", 1), ("report", 0)):
            with self.subTest(mode=mode):
                result = subprocess.run([*command, "--payload-budget-mode=" + mode],
                                        capture_output=True, text=True, check=False,
                                        timeout=test_timing.STALL_GUARD_SECONDS)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertIn("metric=wasm_raw actual=19 limit=1", result.stdout)
                self.assertIn("metric=total_raw", result.stdout)
                self.assertIn("metric=wasm_function_body actual=2 limit=1 delta=1", result.stdout)
                self.assertIn("metric=wasm_data_segments actual=1 limit=0 delta=1", result.stdout)
        for extra in ("--expected-js-property=missingBridge", "--forbidden-js-token=requiredBridge"):
            with self.subTest(contract=extra):
                result = subprocess.run([*command, "--payload-budget-mode=report", extra],
                                        capture_output=True, text=True, check=False,
                                        timeout=test_timing.STALL_GUARD_SECONDS)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        (package / "editor.wasm").write_bytes(b"invalid module")
        result = subprocess.run([*command, "--payload-budget-mode=report"],
                                capture_output=True, text=True, check=False,
                                timeout=test_timing.STALL_GUARD_SECONDS)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("invalid WebAssembly header", result.stderr)

    def test_prove_lockfile_snapshots_the_checked_lockfile(self):
        result, _ = self._run("prove-lockfile", str(self.snapshot))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.snapshot.read_bytes(),
                         (self.repo / "MODULE.bazel.lock").read_bytes())

    def test_first_failing_command_stops_the_lane_with_its_status(self):
        for command, expected in _COMMANDS.items():
            for index in range(len(expected)):
                with self.subTest(command=command, failing=index):
                    result, calls = self._run(*self._args(command), fail_call=index + 1,
                                              fail_status=index + 3)
                    self.assertEqual(result.returncode, index + 3, result.stderr)
                    self.assertEqual(calls, self._expected(command)[:index + 1])
                    self.assertIn("failed (exit %d)" % (index + 3), self.summary.read_text())
                    self.assertIn("::error::", result.stdout)

    def test_a_failed_lockfile_proof_leaves_no_snapshot(self):
        for index in range(3):
            with self.subTest(failing=index):
                result, _ = self._run("prove-lockfile", str(self.snapshot),
                                      fail_call=index + 1, fail_status=1)
                self.assertEqual(result.returncode, 1)
                self.assertFalse(self.snapshot.exists())

    def test_usage_errors_are_rejected_before_any_command(self):
        for args in ((), ("deploy",), ("build", "extra"), ("prefetch", "--config=ci"),
                     ("prove-lockfile",), ("prove-lockfile", ""),
                     ("prove-lockfile", "a", "b")):
            with self.subTest(args=args):
                result, calls = self._run(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(calls, [])
                self.assertIn("editor_wasm_lane.sh prefetch", result.stderr)


if __name__ == "__main__":
    unittest.main()
