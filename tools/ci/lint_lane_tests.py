"""Runs the Lint lane script against recording stand-ins for its checks."""

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

_CHECKS = [
    ["tools/lint.sh"],
    ["python3", "tools/lint_braces_tests.py"],
    ["python3", "tools/check_design_doc_provenance.py"],
    ["python3", "tools/check_private_details.py"],
    ["python3", "tools/check_js_lint_coverage.py"],
    ["python3", "tools/rust_boundary/check_no_rust_dependencies.py", "--blocking", "default"],
]


class LintLaneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.scripts = [Path(resolver.Rlocation("donner/tools/ci/" + name))
                       for name in ("lint_lane.sh", "lane.sh")]
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.recorder = cls.executables.path / "record.py"
        # Records the command and its working directory, then exits with FAIL's status when the
        # command's script is the one named in FAIL (`path=status`).
        cls.recorder.write_text(
            "import json, os, sys\n"
            "command = sys.argv[1:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([command, os.getcwd()]) + '\\n')\n"
            "script = command[1] if command[0] == 'python3' else command[0]\n"
            "failures = dict(item.split('=') for item in os.environ['FAIL'].split())\n"
            "sys.exit(int(failures.get(script, 0)))\n",
            encoding="utf-8",
        )
        (cls.executables.path / "bin").mkdir()
        write_python_stand_in(cls.executables.path / "bin" / "python3", cls.recorder, "python3")
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in ("dirname", "sed"):
            os.symlink(shutil.which(tool), cls.host / tool)
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lint lane ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "ci").mkdir(parents=True)
        for script in self.scripts:
            shutil.copy(script, self.repo / "tools" / "ci" / script.name)
        lint = self.repo / "tools" / "lint.sh"
        write_python_stand_in(lint, self.recorder, "tools/lint.sh")
        self.capture = self.work / "calls.jsonl"
        self.summary = self.work / "summary.md"

    def _run(self, *args, fail=""):
        self.capture.write_text("", encoding="utf-8")
        self.summary.write_text("", encoding="utf-8")
        env = dict(os.environ, CAPTURE=str(self.capture), FAIL=fail,
                   GITHUB_STEP_SUMMARY=str(self.summary),
                   PATH=os.pathsep.join((str(self.executables.path / "bin"), str(self.host))))
        result = subprocess.run([self.bash, str(self.repo / "tools/ci/lint_lane.sh"), *args],
                                cwd=self.work, env=env, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = []
        for line in self.capture.read_text().splitlines():
            command, cwd = json.loads(line)
            self.assertEqual(cwd, str(self.repo.resolve()), command)
            calls.append(command)
        return result, calls

    def test_runs_every_check_in_workflow_order_from_the_repository_root(self):
        result, calls = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, _CHECKS)
        self.assertEqual(result.stdout.count("::group::"), len(_CHECKS))
        self.assertEqual(result.stdout.count("::endgroup::"), len(_CHECKS))
        summary = self.summary.read_text(encoding="utf-8")
        self.assertEqual(summary.count("| passed |"), len(_CHECKS))

    def test_first_failing_check_stops_the_lane_with_its_status(self):
        for index, check in enumerate(_CHECKS):
            script = check[1] if check[0] == "python3" else check[0]
            with self.subTest(script=script):
                result, calls = self._run(fail=f"{script}={index + 3}")
                self.assertEqual(result.returncode, index + 3, result.stderr)
                self.assertEqual(calls, _CHECKS[:index + 1])
                self.assertIn(f"failed (exit {index + 3})", self.summary.read_text())
                self.assertIn("::error::", result.stdout)

    def test_arguments_are_rejected_before_any_check(self):
        result, calls = self._run("donner")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])
        # The usage text is the script's whole header comment, and nothing past it.
        usage = result.stderr.splitlines()
        self.assertEqual(usage[0], "lint_lane.sh - the Lint workflow's checks, in the workflow's "
                         "order, as one command.")
        self.assertIn("  - tools/check_js_lint_coverage.py: every tracked JavaScript file is in "
                      "the ESLint test", usage)
        self.assertEqual(usage[-1], "check ends the lane with its exit status.")


if __name__ == "__main__":
    unittest.main()
