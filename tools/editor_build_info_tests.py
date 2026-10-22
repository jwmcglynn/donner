"""Release metadata must follow explicit commit inputs without reading the developer checkout."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class EditorBuildInfoTest(unittest.TestCase):
    def test_empty_action_environment_supports_default_developer_identity(self):
        resolver = runfiles.Create()
        script = resolver.Rlocation("_main/donner/editor/write_build_info.sh")
        self.assertIsNotNone(script)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module = root / "MODULE.bazel"
            module.write_text('module(\n    name = "donner",\n    version = "0.8.0",\n)\n')
            subprocess.run(["/usr/bin/git", "init", "-q", str(root)], check=True)
            subprocess.run(["/usr/bin/git", "add", "."], cwd=root, check=True)
            subprocess.run(["/usr/bin/git", "-c", "user.name=Test", "-c",
                            "user.email=test@example.invalid", "commit", "-qm", "fixture"],
                           cwd=root, check=True)
            commit = subprocess.check_output(["/usr/bin/git", "rev-parse", "HEAD"],
                                             cwd=root, text=True).strip()
            output = root / "identity.txt"
            subprocess.run(["/bin/bash", script, str(module), str(output), ""],
                           env={"PATH": "/usr/bin:/bin"}, check=True)
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
                subprocess.run(["bash", script, str(module), str(output), commit], check=True)
                self.assertEqual(output.read_text(), "0.8.0\n" + commit + "\n")
            for commit in ("unknown", "c" * 39, "G" * 40):
                result = subprocess.run(["bash", script, str(module), str(output), commit],
                                        capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_text(), "0.8.0\n" + "b" * 40 + "\n")


if __name__ == "__main__":
    unittest.main()
