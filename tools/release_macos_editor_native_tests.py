"""Exercise retained-ZIP packaging with the actual built app on a macOS test executor."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles
from tools import release_macos_editor as native


class NativeEditorArtifactTest(unittest.TestCase):
    def test_actual_signed_app_survives_zip_and_reaches_the_entrypoint(self):
        resolver = runfiles.Create()
        def source(name):
            path = resolver.Rlocation("_main/" + name)
            self.assertIsNotNone(path, name)
            self.assertTrue(Path(path).exists(), name)
            return Path(path)
        with tempfile.TemporaryDirectory(prefix="editor-artifact-") as temporary:
            root = Path(temporary) / "source"
            root.mkdir()
            for name in (*native.INPUTS, "LICENSE"):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source(name), destination)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "commit", "-qm", "artifact test inputs"], cwd=root, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root,
                                              text=True).strip()
            # This isolated fixture tests the producer and consumer, not release qualification.
            (root / "MODULE.bazel.lock").write_text('{}\n')
            app = Path(temporary) / "Donner.app"
            shutil.copytree(source("donner/editor/Donner.app").resolve(), app, symlinks=True)
            output = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", temporary)) / "package"
            result = native.package(
                root=root, app=app,
                notice=source("third_party/licenses/notice_release_editor.txt"),
                compiler_probe=source("tools/ci/release_compiler_identity"), output=output,
                commit=commit, run_id="1", attempt="1")
            self.assertEqual(native.verify(root=root, artifacts=output, commit=commit,
                                           run_id="1", attempt="1"), result)
            print(json.dumps({"archive_sha256": result["archive_sha256"],
                              "archive_size": result["archive_size"],
                              "native_validation": result["native_validation"],
                              "toolchain": result["toolchain"]}, sort_keys=True))


if __name__ == "__main__":
    unittest.main()
