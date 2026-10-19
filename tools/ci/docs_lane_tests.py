"""Runs the docs lane script against recording stand-ins for its tools."""

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

_TOOLS = ("bazelisk", "curl", "python3", "sha256sum", "tar")
_DOXYGEN_URL = ("https://github.com/doxygen/doxygen/releases/download/Release_1_15_0/"
                "doxygen-1.15.0.linux.bin.tar.gz")
_DOXYGEN_SHA256 = "0ec2e5b2c3cd82b7106d19cb42d8466450730b8cb7a9e85af712be38bf4523a1"


class DocsLaneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.scripts = [Path(resolver.Rlocation("donner/tools/ci/" + name))
                       for name in ("docs_lane.sh", "lane.sh")]
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.path / "record.py"
        # Each stand-in records its name, arguments, standard input and working directory, prints
        # a line to standard output, then exits with the status named for it in FAIL (`tool=status`
        # or `tool:last-argument=status`).
        recorder.write_text(
            "import json, os, sys\n"
            "tool, args = sys.argv[1], sys.argv[2:]\n"
            "stdin = sys.stdin.read()\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([tool, *args, stdin, os.getcwd()]) + '\\n')\n"
            "print(tool + ' stdout')\n"
            "failures = dict(item.rsplit('=', 1) for item in os.environ['FAIL'].split())\n"
            "key = tool + ':' + args[-1] if args else tool\n"
            "sys.exit(int(failures.get(key, failures.get(tool, 0))))\n",
            encoding="utf-8",
        )
        (cls.executables.path / "bin").mkdir()
        for tool in _TOOLS:
            write_python_stand_in(cls.executables.path / "bin" / tool, recorder, tool)
        cls.bin = cls.executables.path / "bin"
        # The only host tools the script itself needs, so no host curl, tar or Bazel stands in.
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in ("dirname", "mkdir", "sed"):
            os.symlink(shutil.which(tool), cls.host / tool)
        cls.bash = shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="docs lane ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repo = self.work / "repo"
        (self.repo / "tools" / "ci").mkdir(parents=True)
        for script in self.scripts:
            shutil.copy(script, self.repo / "tools" / "ci" / script.name)
        self.workspace = self.work / "workspace"
        self.temp = self.work / "runner temp"
        self.capture = self.work / "calls.jsonl"
        self.summary = self.work / "summary.md"
        self.github_path = self.work / "path.txt"

    def _run(self, *args, fail="", **env):
        for path in (self.capture, self.summary, self.github_path):
            path.write_text("", encoding="utf-8")
        environ = dict(os.environ, CAPTURE=str(self.capture), FAIL=fail,
                       GITHUB_STEP_SUMMARY=str(self.summary), GITHUB_PATH=str(self.github_path),
                       GITHUB_WORKSPACE=str(self.workspace), RUNNER_TEMP=str(self.temp),
                       PATH=os.pathsep.join((str(self.bin), str(self.host))))
        environ.update(env)
        # Run from elsewhere: the script must find the repository from its own location.
        result = subprocess.run([self.bash, str(self.repo / "tools/ci/docs_lane.sh"), *args],
                                cwd=self.work, env=environ, capture_output=True, text=True,
                                check=False, timeout=test_timing.STALL_GUARD_SECONDS,
                                stdin=subprocess.DEVNULL)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        for call in calls:
            self.assertEqual(call.pop(), str(self.repo.resolve()), call)
        return result, calls

    def test_install_doxygen_verifies_the_pinned_download_before_extracting(self):
        result, calls = self._run("install-doxygen")
        self.assertEqual(result.returncode, 0, result.stderr)
        directory = self.workspace / "doxygen"
        archive = directory / "doxygen.tar.gz"
        # The commands of the previous inline workflow step, in its order.
        self.assertEqual(calls, [
            ["curl", "-fL", _DOXYGEN_URL, "-o", str(archive), ""],
            ["sha256sum", "--check", "--strict", "%s  %s\n" % (_DOXYGEN_SHA256, archive)],
            ["tar", "xzf", str(archive), "-C", str(directory), ""],
        ])
        self.assertTrue(directory.is_dir())
        self.assertEqual(self.github_path.read_text(),
                         "%s/doxygen-1.15.0/bin\n" % directory)

    def test_install_doxygen_never_extracts_an_unverified_download(self):
        order = ["curl", "sha256sum", "tar"]
        for fail, last in (("curl=22", "curl"), ("sha256sum=1", "sha256sum"), ("tar=2", "tar")):
            with self.subTest(fail=fail):
                result, calls = self._run("install-doxygen", fail=fail)
                self.assertEqual(result.returncode, int(fail.split("=")[1]), result.stderr)
                # Nothing runs after the failing command, so a failed check extracts nothing.
                self.assertEqual([call[0] for call in calls], order[:order.index(last) + 1])
                self.assertEqual(self.github_path.read_text(), "")

    def test_install_doxygen_defaults_to_the_repository_outside_actions(self):
        result, calls = self._run("install-doxygen", GITHUB_WORKSPACE="", GITHUB_PATH="")
        self.assertEqual(result.returncode, 0, result.stderr)
        bin_dir = self.repo.resolve() / "doxygen" / "doxygen-1.15.0" / "bin"
        self.assertIn("Add %s to PATH" % bin_dir, result.stdout)
        self.assertEqual(calls[0][4], str(self.repo.resolve() / "doxygen" / "doxygen.tar.gz"))

    def test_record_dependencies_proves_the_lockfile_before_recording_provenance(self):
        result, calls = self._run("record-dependencies")
        self.assertEqual(result.returncode, 0, result.stderr)
        # The commands of the previous inline workflow step, in its order.
        self.assertEqual(calls, [
            ["bazelisk", "mod", "graph", "--ignore_dev_dependency", "--lockfile_mode=update", ""],
            ["bazelisk", "mod", "graph", "--ignore_dev_dependency", "--lockfile_mode=error", ""],
            ["python3", "-B", "-m", "tools.pages_artifact", "provenance",
             "--archive", "%s/artifact.tar" % self.temp, "--lockfile", "MODULE.bazel.lock",
             "--output", "%s/docs-provenance" % self.temp, ""],
        ])
        # The module graph is discarded, as before; the provenance tool's output is logged.
        self.assertNotIn("bazelisk stdout", result.stdout)
        self.assertIn("python3 stdout", result.stdout)
        self.assertEqual(result.stdout.count("::group::"), 3)
        self.assertEqual(result.stdout.count("::endgroup::"), 3)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("### Docs build dependency state", summary)
        self.assertEqual(summary.count("| passed |"), 3)

    def test_record_dependencies_stops_at_the_first_failure(self):
        for fail, count in (("bazelisk:--lockfile_mode=update=7", 1),
                            ("bazelisk:--lockfile_mode=error=48", 2), ("python3=1", 3)):
            with self.subTest(fail=fail):
                result, calls = self._run("record-dependencies", fail=fail)
                status = int(fail.rsplit("=", 1)[1])
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(len(calls), count, calls)
                self.assertIn(f"failed (exit {status})", self.summary.read_text())

    def test_record_dependencies_requires_the_runner_temp_directory(self):
        result, calls = self._run("record-dependencies", RUNNER_TEMP="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RUNNER_TEMP", result.stderr)
        self.assertEqual(calls, [])

    def test_invalid_usage_runs_nothing(self):
        for args in ((), ("bogus",), ("install-doxygen", "extra")):
            with self.subTest(args=args):
                result, calls = self._run(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
