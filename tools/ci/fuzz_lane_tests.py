"""Runs the Fuzz workflow phase script against a recording Bazel stand-in."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.ci import fuzz_lane
from tools.test_executable_fixtures import ExecutableFixtureDirectory


def _result(label, cached_locally=False, cached_remotely=False, strategy="linux-sandbox"):
    result = {"status": "PASSED", "executionInfo": {"strategy": strategy}}
    if cached_locally:
        result["cachedLocally"] = True
    if cached_remotely:
        result["executionInfo"]["cachedRemotely"] = True
    return json.dumps({"id": {"testResult": {"label": label, "run": 1, "shard": 1,
                                             "attempt": 1}},
                       "testResult": result})


_STARTED = json.dumps({"id": {"started": {}}, "started": {"command": "test"}})
_SUMMARY = json.dumps({"id": {"testSummary": {"label": "//a:soak"}},
                       "testSummary": {"overallStatus": "PASSED"}})

# Records its arguments, writes the build events named by $EVENTS to its
# --build_event_json_file, and exits with $STATUS.
_BAZELISK = """\
import json, os, sys
with open(os.environ["CAPTURE"], "w", encoding="utf-8") as capture:
    json.dump([sys.argv[1:], os.getcwd()], capture)
for argument in sys.argv[1:]:
    if argument.startswith("--build_event_json_file="):
        with open(argument.split("=", 1)[1], "w", encoding="utf-8") as events:
            events.write(os.environ["EVENTS"])
sys.exit(int(os.environ["STATUS"]))
"""


class BuildEventTest(unittest.TestCase):
    def test_executed_results_pass(self):
        lines = [_STARTED, _result("//a:soak"), _result("//b:soak", strategy="remote"),
                 _SUMMARY, ""]
        self.assertEqual(fuzz_lane.test_results(lines), [("//a:soak", False), ("//b:soak", False)])
        self.assertEqual(fuzz_lane.problems(lines), [])

    def test_every_cache_kind_fails(self):
        # Bazel reports a disk cache hit as `cachedRemotely` with the strategy "disk cache hit".
        cases = {
            "action cache": _result("//a:soak", cached_locally=True),
            "disk cache": _result("//a:soak", cached_remotely=True, strategy="disk cache hit"),
            "remote cache": _result("//a:soak", cached_remotely=True, strategy="remote cache hit"),
        }
        for name, cached in cases.items():
            with self.subTest(cache=name):
                lines = [_STARTED, _result("//b:soak"), cached, cached]
                self.assertEqual(fuzz_lane.problems(lines),
                                 ["//a:soak was served from a cache instead of executing"])

    def test_no_results_fail(self):
        self.assertEqual(fuzz_lane.problems([_STARTED, ""]),
                         ["the phase reported no test results, so no soak ran"])

    def test_malformed_events_fail(self):
        with self.assertRaisesRegex(ValueError, "line 2 is not a build event"):
            fuzz_lane.problems([_STARTED, "{"])


class FuzzLaneScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = Path(runfiles.Create().Rlocation("donner/tools/ci/fuzz_lane.py"))
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.write("record_bazelisk.py", _BAZELISK)
        cls.executables.write_python_entry("bazelisk", recorder)
        cls.python = cls.executables.write_python3()

    def _run(self, *args, events=(), status=0):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.json"
            env = dict(os.environ, CAPTURE=str(capture), EVENTS="\n".join(events) + "\n",
                       STATUS=str(status), PATH=str(self.executables.path))
            result = subprocess.run([str(self.python), str(self.script), *args], env=env,
                                    capture_output=True, text=True, check=False,
                                    timeout=test_timing.STALL_GUARD_SECONDS)
            call = json.loads(capture.read_text()) if capture.exists() else None
        return result, call

    def test_runs_the_lane_and_accepts_executed_results(self):
        result, (args, cwd) = self._run("fuzz-lane", "//...",
                                        events=[_STARTED, _result("//a:soak")])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(args[:2], ["test", "--config=fuzz-lane"])
        self.assertRegex(args[2], r"^--build_event_json_file=/.+/build_events\.json$")
        self.assertEqual(args[3:], ["--", "//..."])
        self.assertEqual(Path(cwd), self.script.resolve().parents[2])

    def test_cached_result_fails_the_phase_loudly(self):
        result, _ = self._run("fuzz-lane-geode", "//...", events=[
            _result("//a:soak"), _result("//b:soak", cached_remotely=True)])
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error::fuzz-lane-geode: //b:soak was served from a cache instead of "
                      "executing", result.stdout)
        self.assertIn("--nocache_test_results", result.stderr)

    def test_phase_without_results_fails(self):
        result, _ = self._run("fuzz-lane", "//...", events=[_STARTED])
        self.assertEqual(result.returncode, 1)
        self.assertIn("no test results", result.stdout)

    def test_bazel_failure_keeps_its_status(self):
        result, _ = self._run("fuzz-lane", "//...", events=[_result("//a:soak")], status=3)
        self.assertEqual(result.returncode, 3)
        self.assertNotIn("::error::", result.stdout)

    def test_usage_errors_run_nothing(self):
        for args in ((), ("fuzz-lane",), ("--config=fuzz-lane", "//..."),
                     ("fuzz-lane", "--cache_test_results=yes", "//...")):
            with self.subTest(args=args):
                result, call = self._run(*args)
                self.assertEqual(result.returncode, 2)
                self.assertIsNone(call)
                self.assertIn("Usage: tools/ci/fuzz_lane.py LANE PATTERN...", result.stderr)


if __name__ == "__main__":
    unittest.main()
