"""Runs the Coverage lane and the runner-disk cleanup against recording stand-ins."""

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


class LaneScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.scripts = {name: Path(resolver.Rlocation("donner/tools/ci/" + name))
                       for name in ("coverage_lane.sh", "free_runner_disk.sh")}
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.recorder = cls.executables.path / "record.py"
        # Records the command and its working directory. FAIL maps a tool to an exit status, and
        # SKIP_MARKER makes the coverage stand-in leave the all-skipped marker.
        cls.recorder.write_text(
            "import json, os, sys\n"
            "tool, args = sys.argv[1], sys.argv[2:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([tool, *args, os.getcwd()]) + '\\n')\n"
            "if tool == 'tools/coverage.sh' and os.environ.get('SKIP_MARKER'):\n"
            "    os.makedirs('coverage-report', exist_ok=True)\n"
            "    open('coverage-report/coverage_skipped', 'w').write('all_skipped\\n')\n"
            "failures = dict(item.split('=') for item in os.environ['FAIL'].split())\n"
            "sys.exit(int(failures.get(tool, 0)))\n",
            encoding="utf-8")
        (cls.executables.path / "bin").mkdir()
        for tool in ("df", "sudo", "docker"):
            write_python_stand_in(cls.executables.path / "bin" / tool, cls.recorder, tool)
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        os.symlink(shutil.which("dirname"), cls.host / "dirname")
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="coverage lane ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "ci").mkdir(parents=True)
        for name, script in self.scripts.items():
            shutil.copy(script, self.repo / "tools" / "ci" / name)
        write_python_stand_in(self.repo / "tools" / "coverage.sh", self.recorder,
                              "tools/coverage.sh")
        self.capture = self.work / "calls.jsonl"

    def _run(self, script, *args, fail="", **env):
        self.capture.write_text("", encoding="utf-8")
        environ = dict(os.environ, CAPTURE=str(self.capture), FAIL=fail,
                       GITHUB_WORKSPACE="/workspace", RUNNER_TEMP="/runner-temp",
                       PATH=os.pathsep.join((str(self.executables.path / "bin"),
                                             str(self.host))))
        environ.pop("COVERAGE_TARGETS", None)
        environ.update(env)
        result = subprocess.run([self.bash, str(self.repo / "tools/ci" / script), *args],
                                cwd=self.work, env=environ, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = []
        for line in self.capture.read_text().splitlines():
            call = json.loads(line)
            self.assertEqual(call.pop(), str(self.repo.resolve()) if script == "coverage_lane.sh"
                             else str(self.work.resolve()), call)
            calls.append(call)
        return result, calls

    # coverage_lane.sh

    def test_measures_the_targets_from_the_environment_between_disk_reports(self):
        result, calls = self._run("coverage_lane.sh", COVERAGE_TARGETS="//donner/... //examples/...")
        self.assertEqual(result.returncode, 0, result.stderr)
        disk = ["df", "-h", "/", "/workspace", "/runner-temp"]
        self.assertEqual(calls, [disk, ["tools/coverage.sh", "--quiet", "--no-html",
                                        "//donner/...", "//examples/..."], disk])
        self.assertIn("Coverage targets:\n  //donner/...\n  //examples/...\n", result.stdout)
        self.assertEqual(result.stdout.count("::group::"), 2)

    def test_arguments_override_the_environment(self):
        result, calls = self._run("coverage_lane.sh", "//donner/base/...",
                                  COVERAGE_TARGETS="//donner/...")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[1], ["tools/coverage.sh", "--quiet", "--no-html",
                                    "//donner/base/..."])

    def test_an_all_skipped_run_fails_the_lane(self):
        result, _ = self._run("coverage_lane.sh", SKIP_MARKER="1", COVERAGE_TARGETS="//donner/...")
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error::Coverage produced no report", result.stdout)

    def test_a_coverage_failure_stops_the_lane_with_its_status(self):
        result, calls = self._run("coverage_lane.sh", fail="tools/coverage.sh=9",
                                  COVERAGE_TARGETS="//donner/...")
        self.assertEqual(result.returncode, 9)
        self.assertEqual(calls[-1][0], "tools/coverage.sh")

    def test_no_targets_is_a_usage_error(self):
        result, calls = self._run("coverage_lane.sh")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])

    # free_runner_disk.sh

    def test_cleanup_removes_the_unused_toolchains_between_disk_reports(self):
        result, calls = self._run("free_runner_disk.sh", fail="docker=1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [
            ["df", "-h", "/"],
            ["sudo", "rm", "-rf", "/opt/ghc", "/opt/hostedtoolcache/CodeQL", "/usr/local/.ghcup",
             "/usr/local/lib/android", "/usr/local/share/boost", "/usr/share/dotnet",
             "/usr/share/swift"],
            ["docker", "system", "prune", "--all", "--force"],
            ["sudo", "apt-get", "clean"],
            ["df", "-h", "/"],
        ])

    def test_cleanup_failure_fails_the_step(self):
        result, calls = self._run("free_runner_disk.sh", fail="sudo=5")
        self.assertEqual(result.returncode, 5)
        self.assertEqual(calls[-1][:2], ["sudo", "rm"])


if __name__ == "__main__":
    unittest.main()
