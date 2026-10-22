"""Check the actual signed app and ZIP on a macOS executor, without release or GUI qualification."""

import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from python.runfiles import runfiles
from tools import release_macos_editor as native
from tools import macos_editor_test_bundle as bundle


def _fixture_role(app, member):
    relative = member.relative_to(app).as_posix()
    roles = {"Contents/MacOS/Donner": "main-executable", "Contents/Info.plist": "plist"}
    if relative in roles:
        return roles[relative]
    prefixes = {"Contents/_CodeSignature/": "signature", "Contents/Resources/": "resources",
                "Contents/Frameworks/": "frameworks"}
    return next((role for prefix, role in prefixes.items() if relative.startswith(prefix)), "other")


def _fixture_inventory(app):
    counts = {}
    visited = 0
    for parent, directories, files in os.walk(app, followlinks=False):
        for name in directories + files:
            member = Path(parent) / name
            kind = "link" if member.is_symlink() else "directory" if member.is_dir() else "regular"
            key = _fixture_role(app, member) + ":" + kind
            counts[key] = counts.get(key, 0) + 1
            visited += 1
            if visited >= native.MAX_FILES:
                return {"counts": counts, "truncated": True}
    return {"counts": counts, "truncated": False}


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
        print(json.dumps({"signature_stage": stage, "status": "invalid",
                          "reason": str(error)}), flush=True)
    else:
        print(json.dumps({"signature_stage": stage, "status": "valid"}), flush=True)


class NativeEditorArtifactTest(unittest.TestCase):
    def test_archive_preserves_genuine_producer_links_for_rejection(self):
        import tarfile
        resolver = runfiles.Create()
        with tempfile.TemporaryDirectory(prefix="editor-linked-archive-") as temporary:
            base = Path(temporary)
            archive = base / "producer.tar"
            shutil.copy2(resolver.Rlocation("_main/donner/editor/Donner.app.test-data.tar"), archive)
            producer = base / "producer.app"
            bundle.extract_bundle(archive, producer)
            (producer / "Contents/Resources/genuine-link").symlink_to(
                producer / "Contents/Resources/Donner.icns")
            def normalized(member):
                member.uid = member.gid = 0
                member.uname, member.gname = "root", "wheel"
                return member
            linked_archive = base / "linked.tar"
            with tarfile.open(linked_archive, "w", format=tarfile.USTAR_FORMAT) as output:
                output.add(producer / "Contents", arcname="Contents", filter=normalized)
            rejected = base / "rejected.app"
            with self.assertRaisesRegex(ValueError, "regular USTAR"):
                bundle.extract_bundle(linked_archive, rejected)
            self.assertFalse(rejected.exists())

    def test_actual_signed_app_survives_zip_and_reaches_the_entrypoint(self):
        resolver = runfiles.Create()
        def source(name):
            path = resolver.Rlocation("_main/" + name)
            self.assertIsNotNone(path, name)
            self.assertTrue(Path(path).exists(), name)
            original = Path(path)
            linked = Path(temporary) / "linked-runfiles" / name
            linked.parent.mkdir(parents=True, exist_ok=True)
            if not linked.is_symlink():
                linked.symlink_to(original.resolve(), target_is_directory=original.is_dir())
            self.assertTrue(linked.is_symlink(), name)
            destination = Path(temporary) / "materialized-runfiles" / name
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(linked, destination)
            self.assertFalse(destination.is_symlink(), name)
            self.assertEqual(destination.stat().st_mode & 0o777, original.stat().st_mode & 0o777)
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
            archive_input = source("donner/editor/Donner.app.test-data.tar")
            bundle.extract_bundle(archive_input, app)
            _verify_fixture_bundle(app, "extracted-producer")
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
