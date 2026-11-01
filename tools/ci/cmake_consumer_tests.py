"""Runs the CMake consumer lane script against recording stand-ins for its tools."""

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

_TOOLS = ("bazel", "brew", "ccache", "cmake", "ctest", "ninja", "python3")


class CMakeConsumerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.scripts = [Path(resolver.Rlocation("donner/tools/ci/" + name))
                       for name in ("cmake_consumer.sh", "lane.sh")]
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.path / "record.py"
        # Each stand-in records its name, arguments and working directory, then exits with the
        # status named for it in FAIL (`tool=status`, or `tool:first-argument=status`).
        recorder.write_text(
            "import json, os, sys\n"
            "tool, args = sys.argv[1], sys.argv[2:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([tool, *args, os.getcwd()]) + '\\n')\n"
            "failures = dict(item.split('=') for item in os.environ['FAIL'].split())\n"
            "key = tool + ':' + args[0] if args else tool\n"
            "sys.exit(int(failures.get(key, failures.get(tool, 0))))\n",
            encoding="utf-8",
        )
        (cls.executables.path / "bin").mkdir()
        for tool in _TOOLS:
            write_python_stand_in(cls.executables.path / "bin" / tool, recorder, tool)
        cls.bin = cls.executables.path / "bin"
        # The only host tools the script itself needs. Keeping every other host directory off
        # PATH means a runner's own cmake, ccache or ninja can never stand in for a fake.
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in ("dirname", "sed"):
            os.symlink(shutil.which(tool), cls.host / tool)
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="cmake consumer ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "ci").mkdir(parents=True)
        for script in self.scripts:
            shutil.copy(script, self.repo / "tools" / "ci" / script.name)
        self.capture = self.work / "calls.jsonl"
        self.summary = self.work / "summary.md"

    def _run(self, *args, path=None, fail="", **env):
        self.capture.write_text("", encoding="utf-8")
        self.summary.write_text("", encoding="utf-8")
        environ = dict(os.environ, CAPTURE=str(self.capture), FAIL=fail,
                       GITHUB_STEP_SUMMARY=str(self.summary),
                       PATH=os.pathsep.join((path or str(self.bin), str(self.host))))
        environ.update(env)
        # Run from elsewhere: the script must find the repository from its own location.
        result = subprocess.run([self.bash, str(self.repo / "tools/ci/cmake_consumer.sh"), *args],
                                cwd=self.work, env=environ, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        for call in calls:
            self.assertEqual(call.pop(), str(self.repo.resolve()), call)
        return result, calls

    def _configure(self, *extra):
        return ["cmake", "-S", "examples/cmake_consumer", "-B", "build", "-G", "Ninja",
                f"-DDONNER_SOURCE_DIR={self.repo.resolve()}",
                "-DDONNER_RENDERER_BACKEND=tiny_skia", *extra]

    def test_ccache_build_runs_the_hosted_cmake_workflow_commands_in_order(self):
        result, calls = self._run("build", "--ccache", CCACHE_MAXSIZE="750M")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [
            ["ccache", "--zero-stats"],
            ["ccache", "--max-size=750M"],
            ["python3", "tools/cmake/gen_cmakelists.py"],
            self._configure("-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"),
            ["cmake", "--build", "build", "--target", "donner_cmake_consumer", "--parallel"],
            ["ctest", "--test-dir", "build", "--output-on-failure"],
            ["ccache", "--show-stats"],
        ])
        self.assertEqual(result.stdout.count("::group::"), 6)
        self.assertEqual(result.stdout.count("::endgroup::"), 6)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("| Test | passed |", summary)
        self.assertNotIn("failed", summary)

    def test_checked_build_validates_first_and_honors_parallelism(self):
        result, calls = self._run("build", "--check", "--parallel", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [
            ["python3", "tools/cmake/gen_cmakelists.py", "--check"],
            ["python3", "tools/cmake/gen_cmakelists.py"],
            self._configure(),
            ["cmake", "--build", "build", "--target", "donner_cmake_consumer", "--parallel", "2"],
            ["ctest", "--test-dir", "build", "--output-on-failure"],
        ])

    def test_bazel_server_is_released_after_generation_before_four_worker_build(self):
        result, calls = self._run("build", "--check", "--release-bazel-server", "--parallel", "4")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [
            ["python3", "tools/cmake/gen_cmakelists.py", "--check"],
            ["python3", "tools/cmake/gen_cmakelists.py"],
            ["bazel", "shutdown"],
            self._configure(),
            ["cmake", "--build", "build", "--target", "donner_cmake_consumer", "--parallel", "4"],
            ["ctest", "--test-dir", "build", "--output-on-failure"],
        ])

    def test_failed_shutdown_stops_before_native_compilation(self):
        result, calls = self._run("build", "--release-bazel-server", "--parallel", "4",
                                  fail="bazel=7")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(calls[-1], ["bazel", "shutdown"])
        self.assertNotIn("cmake", [call[0] for call in calls])

    def test_a_failing_phase_stops_the_lane_with_its_status(self):
        for fail, last in (("ctest=8", "ctest"), ("cmake:--build=3", "cmake"),
                           ("python3=5", "python3"), ("ccache:--zero-stats=6", "ccache")):
            with self.subTest(fail=fail):
                result, calls = self._run("build", "--ccache", fail=fail, CCACHE_MAXSIZE="1G")
                status = int(fail.rsplit("=", 1)[1])
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(calls[-1][0], last, calls)
                self.assertNotIn(["ctest", "--test-dir", "build", "--output-on-failure"],
                                 calls[:-1])
                self.assertIn(f"failed (exit {status})", self.summary.read_text())
                self.assertIn("failed with exit status %d" % status, result.stdout)
                self.assertEqual(result.stdout.count("::group::"),
                                 result.stdout.count("::endgroup::"))

    def test_ccache_requires_a_size_cap(self):
        result, calls = self._run("build", "--ccache", CCACHE_MAXSIZE="")
        self.assertEqual(result.returncode, 2)
        self.assertIn("CCACHE_MAXSIZE", result.stderr)
        self.assertEqual(calls, [])

    def test_install_tools_installs_only_missing_tools(self):
        only = self.work / "only"
        only.mkdir()
        for tool in ("brew", "ccache"):
            os.symlink(self.bin / tool, only / tool)
        result, calls = self._run("install-tools", path=str(only))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [["brew", "install", "ninja"]])

        result, calls = self._run("install-tools")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])

    def test_invalid_usage_runs_nothing(self):
        for args in ((), ("bogus",), ("build", "--parallel"), ("build", "--jobs"),
                     ("install-tools", "extra")):
            with self.subTest(args=args):
                result, calls = self._run(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
