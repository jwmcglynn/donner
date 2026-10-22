"""Check the actual signed app and ZIP on a macOS executor, without release or GUI qualification."""

import json
import os
from pathlib import Path
import shutil
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
        commit = os.environ.get("DONNER_RELEASE_SOURCE_COMMIT")
        self.assertIsNotNone(commit, "the caller must independently select the source commit")
        with tempfile.TemporaryDirectory(prefix="editor-artifact-") as temporary:
            root = Path(temporary) / "source"
            root.mkdir()
            for name in (*native.INPUTS, "LICENSE"):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source(name), destination)
            build_info = source("donner/editor/editor_build_info.txt")
            identity = native.check_build_info(root, build_info, commit)
            app = Path(temporary) / "Donner.app"
            shutil.copytree(source("donner/editor/Donner.app").resolve(), app, symlinks=True)
            output = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", temporary)) / "package"
            output.mkdir()
            archive = output / native.ARCHIVE
            files = native.create_validated_archive(
                root, app, source("third_party/licenses/notice_release_editor.txt"), archive, commit)
            self.assertTrue(native.REQUIRED <= files.keys())
            print(json.dumps({"source_commit": commit, "build_info_sha256": identity,
                              "archive_sha256": native.release_cli.digest(archive),
                              "archive_size": archive.stat().st_size,
                              "native_validation": native.NATIVE_CHECKS,
                              "toolchain": native._native_toolchain(root),
                              "compiler_version": native.release_cli.compiler_version(
                                  source("tools/ci/release_compiler_identity")),
                              "qualification": "artifact contract, not release provenance or GUI render"},
                             sort_keys=True))


if __name__ == "__main__":
    unittest.main()
