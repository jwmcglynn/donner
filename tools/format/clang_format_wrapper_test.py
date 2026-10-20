"""Runs tools/format/clang_format.sh against a fake Bazel and a fake built clang-format."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory


class ClangFormatWrapperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = Path(runfiles.Create().Rlocation("donner/tools/format/clang_format.sh"))
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        fake_bazel = cls.executables.path / "fake_bazel.py"
        # Records each Bazel command and answers cquery and info from the environment.
        fake_bazel.write_text(
            "import json, os, sys\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([args, os.getcwd()]) + '\\n')\n"
            "if 'cquery' in args:\n"
            "    print('bazel-out/unbuilt/bin/clang-format')\n"
            "    print('external/llvm/bin/clang-format')\n"
            "elif 'info' in args:\n"
            "    print(os.environ['EXECROOT'] if 'execution_root' in args else os.environ['OUTPUT_BASE'])\n"
            "sys.exit(int(os.environ.get('BUILD_STATUS', '0')) if 'build' in args else 0)\n",
            encoding="utf-8")
        cls.executables.write("bazel", "#!/bin/sh\nexec %s %s \"$@\"\n" % (
            shlex.quote(sys.executable), shlex.quote(str(fake_bazel))))
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="clang format wrapper ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "format").mkdir(parents=True)
        shutil.copy(self.script, self.repo / "tools" / "format" / "clang_format.sh")
        self.capture = self.work / "bazel.jsonl"
        self.output_base = self.work / "output base"
        binary = self.output_base / "external" / "llvm" / "bin" / "clang-format"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/bin/sh\nprintf 'formatter:%s\\n' \"$PWD\" \"$@\"\n", encoding="utf-8")
        binary.chmod(0o755)

    def _run(self, *args, **env):
        self.capture.write_text("", encoding="utf-8")
        environ = dict(os.environ, CAPTURE=str(self.capture), EXECROOT=str(self.work / "none"),
                       OUTPUT_BASE=str(self.output_base),
                       PATH=f"{self.executables.path}{os.pathsep}{os.environ.get('PATH', '')}")
        environ.update(env)
        caller = self.work / "caller"
        caller.mkdir(exist_ok=True)
        result = subprocess.run([self.bash, str(self.repo / "tools/format/clang_format.sh"), *args],
                                cwd=caller, env=environ, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        return result, calls

    def test_builds_the_pinned_binary_and_runs_it_from_the_callers_directory(self):
        result, calls = self._run("-i", "relative/file.cc")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            f"formatter:{(self.work / 'caller').resolve()}", "formatter:-i",
            "formatter:relative/file.cc"])
        self.assertEqual([args[:2] for args, _ in calls][:2],
                         [["build", "--noshow_progress"], ["cquery", "--noshow_progress"]])
        self.assertIn("//tools/format:clang_format", calls[0][0])
        for _, cwd in calls:
            self.assertEqual(cwd, str(self.repo.resolve()))

    def test_startup_flags_come_from_donner_bazel(self):
        result, calls = self._run("--version", DONNER_BAZEL="bazel --output_user_root=/x")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(all(args[0] == "--output_user_root=/x" for args, _ in calls), calls)

    def test_blank_donner_bazel_falls_back_to_bazel(self):
        result, calls = self._run("--version", DONNER_BAZEL="  ")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0][0][0], "build")

    def test_build_failure_stops_before_running_anything(self):
        result, calls = self._run("--version", BUILD_STATUS="3")
        self.assertEqual(result.returncode, 3)
        self.assertEqual(result.stdout, "")
        self.assertEqual(len(calls), 1)

    def test_missing_binary_fails_with_its_path(self):
        result, _ = self._run("--version", OUTPUT_BASE=str(self.work / "elsewhere"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("external/llvm/bin/clang-format", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
