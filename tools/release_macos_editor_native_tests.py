"""Check the actual signed app and ZIP on a macOS executor, without release or GUI qualification."""

import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from python.runfiles import runfiles
from tools import release_macos_editor as native


def _fixture_member(app, member):
    relative = member.relative_to(app).as_posix()
    try:
        native._safe_name(native.BUNDLE + "/" + relative)
    except ValueError:
        relative = "unexpected-name"
    kind = "link" if member.is_symlink() else "directory" if member.is_dir() else "regular"
    return {"member": relative, "type": kind}


def _fixture_inventory(app):
    members = []
    for parent, directories, files in os.walk(app, followlinks=False):
        for name in sorted(directories + files):
            member = Path(parent) / name
            members.append(_fixture_member(app, member))
            if len(members) >= native.MAX_FILES:
                return {"members": members, "truncated": True}
    return {"members": members, "truncated": False}


def _verify_fixture_bundle(app, stage):
    required = ("Contents/Info.plist", "Contents/MacOS/Donner",
                "Contents/Resources/Donner.icns", "Contents/_CodeSignature/CodeResources")
    print(json.dumps({"signature_stage": stage, "root_is_link": app.is_symlink(),
                      "required_link_count": sum((app / name).is_symlink() for name in required),
                      "inventory": _fixture_inventory(app)}),
          flush=True)
    try:
        native._tool("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    except ValueError as error:
        raise ValueError(stage + ": " + str(error)) from None


class NativeEditorArtifactTest(unittest.TestCase):
    def test_leaf_symlink_runfiles_are_not_silently_dereferenced(self):
        resolver = runfiles.Create()
        original = Path(resolver.Rlocation("_main/donner/editor/Donner.app")).resolve()
        with tempfile.TemporaryDirectory(prefix="editor-linked-runfiles-") as temporary:
            linked = Path(temporary) / "linked.app"
            for parent, directories, files in os.walk(original, followlinks=False):
                destination = linked / Path(parent).relative_to(original)
                destination.mkdir(parents=True, exist_ok=True)
                for name in files:
                    (destination / name).symlink_to((Path(parent) / name).resolve())
            copied = Path(temporary) / "copied.app"
            shutil.copytree(linked, copied, symlinks=True)
            try:
                _verify_fixture_bundle(copied, "simulated-leaf-symlink-copy")
            except ValueError as error:
                print(str(error), flush=True)
            license_path = Path(temporary) / "LICENSE"
            license_path.write_text("fixture license")
            notice = Path(temporary) / "NOTICE.txt"
            notice.write_text("fixture notice")
            with self.assertRaisesRegex(ValueError, "linked|symlink"):
                native._bundle_files(copied, license_path, notice)

    def test_actual_signed_app_survives_zip_and_reaches_the_entrypoint(self):
        resolver = runfiles.Create()
        def source(name):
            path = resolver.Rlocation("_main/" + name)
            self.assertIsNotNone(path, name)
            self.assertTrue(Path(path).exists(), name)
            original = Path(path)
            if name == "donner/editor/Donner.app":
                _verify_fixture_bundle(original.resolve(), "source-runfile")
            linked = Path(temporary) / "linked-runfiles" / name
            linked.parent.mkdir(parents=True, exist_ok=True)
            if not linked.is_symlink():
                linked.symlink_to(original.resolve(), target_is_directory=original.is_dir())
            self.assertTrue(linked.is_symlink(), name)
            destination = Path(temporary) / "materialized-runfiles" / name
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                if linked.is_dir():
                    shutil.copytree(linked.resolve(), destination, symlinks=True)
                else:
                    shutil.copy2(linked, destination)
            self.assertFalse(destination.is_symlink(), name)
            self.assertEqual(destination.stat().st_mode & 0o777, original.stat().st_mode & 0o777)
            if name == "donner/editor/Donner.app":
                _verify_fixture_bundle(destination, "materialized-runfile")
            return destination
        commit = os.environ.get("DONNER_RELEASE_SOURCE_COMMIT")
        self.assertIsNotNone(commit, "the caller must independently select the source commit")
        with tempfile.TemporaryDirectory(prefix="editor-artifact-") as temporary:
            root = Path(temporary) / "source"
            root.mkdir()
            for name in (*native.INPUTS, "LICENSE"):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source(name), destination)
            build_info = Path(temporary) / "editor_build_info.txt"
            shutil.copyfile(source("donner/editor/editor_build_info.txt"), build_info)
            identity = native.check_build_info(root, build_info, commit)
            app = Path(temporary) / "Donner.app"
            shutil.copytree(source("donner/editor/Donner.app").resolve(), app, symlinks=True)
            _verify_fixture_bundle(app, "copied-app")
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
