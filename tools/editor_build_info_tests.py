"""Release metadata must follow explicit commit inputs without reading the developer checkout."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class EditorBuildInfoTest(unittest.TestCase):
    def setUp(self):
        self.tools = {}
        for name in ("bash", "git", "sed", "head", "dirname", "basename"):
            executable = shutil.which(name)
            self.assertIsNotNone(executable, "configured test tool is missing: " + name)
            self.tools[name] = executable
        directories = dict.fromkeys(str(Path(tool).parent) for tool in self.tools.values())
        self.environment = {"PATH": os.pathsep.join(directories)}

    def test_empty_action_environment_supports_default_developer_identity(self):
        resolver = runfiles.Create()
        script = resolver.Rlocation("_main/donner/editor/write_build_info.sh")
        self.assertIsNotNone(script)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module = root / "MODULE.bazel"
            module.write_text('module(\n    name = "donner",\n    version = "0.8.0",\n)\n')
            subprocess.run([self.tools["git"], "init", "-q", str(root)],
                           env=self.environment, check=True)
            subprocess.run([self.tools["git"], "add", "."], cwd=root,
                           env=self.environment, check=True)
            subprocess.run([self.tools["git"], "-c", "user.name=Test", "-c",
                            "user.email=test@example.invalid", "commit", "-qm", "fixture"],
                           cwd=root, env=self.environment, check=True)
            commit = subprocess.check_output([self.tools["git"], "rev-parse", "HEAD"],
                                             cwd=root, env=self.environment, text=True).strip()
            output = root / "identity.txt"
            subprocess.run([self.tools["bash"], script, str(module), str(output), ""],
                           env=self.environment, check=True)
            self.assertEqual(output.read_text(), "0.8.0\n" + commit + "\n")

    def test_same_module_and_script_emit_distinct_explicit_commit_identities(self):
        resolver = runfiles.Create()
        script = resolver.Rlocation("_main/donner/editor/write_build_info.sh")
        self.assertIsNotNone(script)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module = root / "MODULE.bazel"
            module.write_text('module(\n    name = "donner",\n    version = "0.8.0",\n)\n')
            output = root / "identity.txt"
            for commit in ("a" * 40, "b" * 40):
                subprocess.run([self.tools["bash"], script, str(module), str(output), commit],
                               env=self.environment, check=True)
                self.assertEqual(output.read_text(), "0.8.0\n" + commit + "\n")
            for commit in ("unknown", "c" * 39, "G" * 40):
                result = subprocess.run([self.tools["bash"], script, str(module), str(output), commit],
                                        env=self.environment, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_text(), "0.8.0\n" + "b" * 40 + "\n")


if __name__ == "__main__":
    unittest.main()
