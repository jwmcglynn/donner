"""Runs the Editor WASM lane script against recording stand-ins for Bazel and its checks."""

import json
import os
from pathlib import Path
import shutil
import subprocess
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
        ["bazelisk", "fetch", "--config=editor-wasm", "//tools/ci:editor_wasm_size_tests"],
        ["bazelisk", "fetch", "--config=wasm-geode", *_GEODE_MODULE_TARGETS],
    ],
    "build": [
        ["bazelisk", "test", "--test_output=errors", "//tools/ci:editor_wasm_audits"],
        ["bazelisk", "test", "--config=editor-wasm", "--test_output=errors",
         "//tools/ci:editor_wasm_shipped_browser_audit"],
        ["bazelisk", "test", "--config=wasm-size", "--test_output=errors",
         "//tools/ci:wasm_size_tests"],
        ["bazelisk", "test", "--config=editor-wasm", "--test_output=errors",
         "--remote_download_outputs=all", "//tools/ci:editor_wasm_size_tests"],
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

    def _run(self, *args, fail_call=0, fail_status=0):
        self.capture.write_text("", encoding="utf-8")
        self.summary.write_text("", encoding="utf-8")
        env = dict(os.environ, CAPTURE=str(self.capture), FAIL_CALL=str(fail_call),
                   FAIL_STATUS=str(fail_status), GITHUB_STEP_SUMMARY=str(self.summary),
                   PATH=os.pathsep.join((str(self.executables.path / "bin"), str(self.host))))
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
