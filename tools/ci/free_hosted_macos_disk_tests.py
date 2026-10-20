"""Runs the hosted macOS disk cleanup against a fake Applications directory and recording tools."""

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


class FreeHostedMacosDiskTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = Path(runfiles.Create().Rlocation(
            "donner/tools/ci/free_hosted_macos_disk.sh"))
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.recorder = cls.executables.path / "record.py"
        # Records each call. `df -Pk` answers with the next free-space value from DF_KIB, and
        # FAIL maps a tool to an exit status.
        cls.recorder.write_text(
            "import json, os, sys\n"
            "tool, args = sys.argv[1], sys.argv[2:]\n"
            "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
            "    capture.write(json.dumps([tool, *args]) + '\\n')\n"
            "if tool == 'df' and args[:1] == ['-Pk']:\n"
            "    counter = os.environ['CAPTURE'] + '.df'\n"
            "    seen = int(open(counter).read()) if os.path.exists(counter) else 0\n"
            "    open(counter, 'w').write(str(seen + 1))\n"
            "    kib = os.environ['DF_KIB'].split()[seen]\n"
            "    print('Filesystem 1024-blocks Used Available Capacity Mounted on')\n"
            "    print('/dev/disk1 100 0 %s 1%% /' % kib)\n"
            "failures = dict(item.split('=') for item in os.environ['FAIL'].split())\n"
            "sys.exit(int(failures.get(tool, 0)))\n",
            encoding="utf-8")
        cls.bin = cls.executables.path / "bin"
        cls.bin.mkdir()
        for tool in ("df", "sudo", "xcrun", "lsregister"):
            write_python_stand_in(cls.bin / tool, cls.recorder, tool)
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in ("awk", "dirname"):
            os.symlink(shutil.which(tool), cls.host / tool)
        # The image's /bin/bash is 3.2, which the script must support.
        cls.bash = "/bin/bash" if os.path.exists("/bin/bash") else shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hosted disk ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.applications = self.work / "Applications"
        # The xcode-27 image's layout: three bundles and their version symlinks.
        for bundle in ("Xcode_27.app", "Xcode_27.1_Release_Candidate.app",
                       "Xcode_27.2_beta_2.app"):
            (self.applications / bundle / "Contents" / "Developer").mkdir(parents=True)
        for link, target in (("Xcode.app", "Xcode_27.app"), ("Xcode_27.0.app", "Xcode_27.app"),
                             ("Xcode_27.1.app", "Xcode_27.1_Release_Candidate.app"),
                             ("Xcode_27.2.app", "Xcode_27.2_beta_2.app")):
            os.symlink(target, self.applications / link)
        self.capture = self.work / "calls.jsonl"

    def _run(self, *args, fail="", **env):
        self.capture.write_text("", encoding="utf-8")
        environ = {
            "CAPTURE": str(self.capture), "FAIL": fail, "DF_KIB": "28311552 62914560",
            "PATH": os.pathsep.join((str(self.bin), str(self.host))),
            "RUNNER_ENVIRONMENT": "github-hosted", "RUNNER_OS": "macOS",
            "DEVELOPER_DIR": str(self.applications / "Xcode_27.1.app" / "Contents" / "Developer"),
            "XCODE_APPLICATIONS_DIR": str(self.applications),
            "LSREGISTER": str(self.bin / "lsregister"),
        }
        environ.update(env)
        result = subprocess.run([self.bash, str(self.script), *args], cwd=self.work, env=environ,
                                capture_output=True, text=True, check=False,
                                timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        return result, calls

    def test_removes_every_other_xcode_and_the_simulator_runtimes(self):
        result, calls = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        apps = self.applications
        self.assertEqual(calls, [
            ["df", "-h", "/"],
            ["df", "-Pk", "/"],
            ["lsregister", "-u", str(apps / "Xcode_27.2_beta_2.app")],
            ["lsregister", "-u", str(apps / "Xcode_27.app")],
            ["sudo", "rm", "-rf", str(apps / "Xcode_27.2_beta_2.app"), str(apps / "Xcode_27.app"),
             str(apps / "Xcode.app"), str(apps / "Xcode_27.0.app"), str(apps / "Xcode_27.2.app")],
            ["sudo", "xcode-select", "--switch",
             str(apps / "Xcode_27.1.app" / "Contents" / "Developer")],
            ["xcrun", "simctl", "runtime", "delete", "all"],
            ["df", "-h", "/"],
            ["df", "-Pk", "/"],
        ])
        self.assertIn("Freed 33 GiB: 27 GiB free before, 60 GiB after.", result.stdout)

    def test_with_only_the_selected_xcode_nothing_is_removed(self):
        for name in ("Xcode_27.app", "Xcode_27.2_beta_2.app", "Xcode.app", "Xcode_27.0.app",
                     "Xcode_27.2.app"):
            path = self.applications / name
            if path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path)
        result, calls = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls, [
            ["df", "-h", "/"],
            ["df", "-Pk", "/"],
            ["sudo", "xcode-select", "--switch",
             str(self.applications / "Xcode_27.1.app" / "Contents" / "Developer")],
            ["xcrun", "simctl", "runtime", "delete", "all"],
            ["df", "-h", "/"],
            ["df", "-Pk", "/"],
        ])

    def test_a_runtime_that_cannot_be_deleted_only_warns(self):
        result, _ = self._run(fail="xcrun=1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::Could not delete the image's simulator runtimes.", result.stdout)

    def test_a_failed_removal_fails_the_step(self):
        for tool in ("lsregister", "sudo"):
            with self.subTest(tool=tool):
                result, calls = self._run(fail=tool + "=5")
                self.assertEqual(result.returncode, 5)
                self.assertNotIn(["xcrun", "simctl", "runtime", "delete", "all"], calls)

    def test_refuses_to_run_outside_a_hosted_macos_runner(self):
        for name, env in {
                "self-hosted": dict(RUNNER_ENVIRONMENT="self-hosted"),
                "unknown environment": dict(RUNNER_ENVIRONMENT=""),
                "Linux": dict(RUNNER_OS="Linux"),
                "no selected Xcode": dict(DEVELOPER_DIR=""),
                "missing selected Xcode": dict(DEVELOPER_DIR=str(self.work / "missing")),
                "selection outside a bundle": dict(DEVELOPER_DIR=str(self.work))}.items():
            with self.subTest(case=name):
                result, calls = self._run(**env)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(calls, [])
                self.assertIn("::error::", result.stdout)

    def test_rejects_arguments(self):
        result, calls = self._run("--all")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
