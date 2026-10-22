"""Contracts for a retained native editor artifact, separate from the Wasm editor."""

import unittest

from tools import bcr_preflight, release_candidate


class NativeEditorReleaseContractTest(unittest.TestCase):
    def test_native_editor_is_required_by_the_preflight_and_release_record(self):
        self.assertIn("editor-macos", release_candidate.RELEASE_ROLES)
        self.assertIn("editor-macos", release_candidate.PREFLIGHT_ROLES)
        self.assertIn("editor-macos", release_candidate.LOCK_ROLES)
        self.assertNotIn("editor-macos", release_candidate.CLI_PLATFORMS)

    def test_every_native_provenance_input_selects_full_preflight(self):
        from tools import release_macos_editor
        for path in (*release_macos_editor.INPUTS, "LICENSE"):
            with self.subTest(path=path):
                self.assertTrue(bcr_preflight.is_release_path(path))

    def test_existing_macos_attempt_builds_and_packages_the_native_app(self):
        plan = bcr_preflight.cli_plan(bcr_preflight.Layout("/fixture"), "darwin-arm64",
                                      "a" * 40, "123", "2")
        commands = [item.argv for item in plan]
        self.assertTrue(any("//donner/editor:editor_macos_app" in argv for argv in commands))
        self.assertTrue(any("tools.release_macos_editor" in argv and "package" in argv
                            for argv in commands))


class MacosEditorPackageTest(unittest.TestCase):
    def setUp(self):
        import tempfile
        from pathlib import Path
        from tools import release_macos_editor as native
        self.native = native
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "source"
        self.root.mkdir()
        for name in native.release_cli.INPUTS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("8.8.1\n" if name == ".bazelversion" else "fixture\n")
        (self.root / "LICENSE").write_text("Donner license fixture\n")
        pin = self.root / "tools/ci_macos_xcode.txt"
        pin.parent.mkdir(parents=True)
        pin.write_text("27.1 27A9275\n")
        (self.root / "MODULE.bazel.lock").write_text('{}\n')
        self.app = self.base / "Donner.app"
        import plistlib
        for name in native.REQUIRED - {"LICENSE", "NOTICE.txt"}:
            path = self.app / name.split("/", 1)[1]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"signed fixture bytes")
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps({
            "CFBundleExecutable": "Donner", "CFBundleIdentifier": "dev.donner.editor",
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.8",
            "CFBundleVersion": "1"}))
        (self.app / "Contents/MacOS/Donner").chmod(0o755)
        self.notice = self.base / "notice.txt"
        self.notice.write_text(native.release_cli.NOTICE_HEADER + "editor\n\nPackage: fixture\n")
        source_plist = self.root / "donner/editor/art/Info.plist"
        source_plist.parent.mkdir(parents=True)
        source_plist.write_bytes((self.app / "Contents/Info.plist").read_bytes())
        import subprocess
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                        "commit", "-qm", "fixture"], cwd=self.root, check=True)
        self.commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.root,
                                              text=True).strip()
        self.output = self.base / "artifacts"

    def package(self):
        from unittest import mock
        with mock.patch.object(self.native, "validate_native") as validate, \
             mock.patch.object(self.native, "_check_entrypoint"), \
             mock.patch.object(self.native, "_native_toolchain", return_value={
                 "xcode": "27.1 27A9275", "macos_sdk": "27.1"}), \
             mock.patch.object(self.native.release_cli, "compiler_version",
                               return_value="Apple clang 17"):
            result = self.native.package(root=self.root, app=self.app, notice=self.notice,
                                         compiler_probe=self.base / "probe", output=self.output,
                                         commit=self.commit, run_id="123", attempt="2")
            self.assertEqual(validate.call_count, 2)
            return result

    def verify(self, **changes):
        options = dict(root=self.root, artifacts=self.output, commit=self.commit,
                       run_id="123", attempt="2")
        options.update(changes)
        return self.native.verify(**options)

    def test_signed_bytes_modes_licenses_and_provenance_roundtrip(self):
        import zipfile
        before = (self.app / "Contents/MacOS/Donner").read_bytes()
        expected = self.package()
        self.assertEqual(self.verify(), expected)
        self.assertEqual(expected["native_validation"]["signature"], "ad-hoc")
        with zipfile.ZipFile(self.output / self.native.ARCHIVE) as archive:
            self.assertEqual(archive.read("Donner SVG Editor.app/Contents/MacOS/Donner"), before)
            self.assertEqual(archive.read("LICENSE"), (self.root / "LICENSE").read_bytes())
            self.assertEqual(archive.read("NOTICE.txt"), self.notice.read_bytes())
            self.assertEqual((archive.getinfo("Donner SVG Editor.app/Contents/MacOS/Donner").external_attr
                              >> 16) & 0o777, 0o755)

    def test_other_attempt_or_commit_is_refused(self):
        self.package()
        for change in ({"attempt": "3"}, {"commit": "f" * 40}, {"run_id": "124"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify(**change)

    def test_archive_mutation_and_unexpected_sidecars_are_refused(self):
        self.package()
        archive = self.output / self.native.ARCHIVE
        original = archive.read_bytes()
        archive.write_bytes(original + b"changed")
        with self.assertRaises(ValueError):
            self.verify()
        archive.write_bytes(original)
        (self.output / "unexpected").write_text("not bound")
        with self.assertRaises(ValueError):
            self.verify()

    def test_plist_bytes_must_equal_the_committed_bundle_metadata(self):
        path = self.app / "Contents/Info.plist"
        path.write_bytes(path.read_bytes().replace(b"<string>1</string>", b"<string>2</string>"))
        with self.assertRaisesRegex(ValueError, "Info.plist"):
            self.package()

    def test_bundle_symlinks_and_nonregular_members_are_refused(self):
        (self.app / "Contents/Resources/link").symlink_to(self.notice)
        with self.assertRaisesRegex(ValueError, "symlink|linked"):
            self.package()

    def _extra_entry(self, name, mode=0o100644, data=b"fixture"):
        import zipfile
        with zipfile.ZipFile(self.output / self.native.ARCHIVE, "a") as archive:
            item = zipfile.ZipInfo(name)
            item.external_attr = mode << 16
            archive.writestr(item, data)

    def test_traversal_absolute_backslash_and_case_collision_are_refused(self):
        self.package()
        path = self.output / self.native.ARCHIVE
        original = path.read_bytes()
        for name in ("../escape", "/escape", "Donner SVG Editor.app/../escape", "Donner SVG Editor.app\\escape",
                     "Donner SVG Editor.app/Contents/MacOS/donner", "Donner SVG Editor.app/Contents/MacOS/Donner",
                     "Donner SVG Editor.app/contents/Resources/extra",
                     "Donner SVG Editor.app/Contents/Resources/arbitrary space"):
            with self.subTest(name=name):
                path.write_bytes(original)
                self._extra_entry(name)
                with self.assertRaises(ValueError):
                    self.native.inspect_archive(path)

    def test_link_and_special_modes_are_refused(self):
        self.package()
        path = self.output / self.native.ARCHIVE
        original = path.read_bytes()
        for mode in (0o120777, 0o010644, 0o100777, 0o104755):
            with self.subTest(mode=mode):
                path.write_bytes(original)
                self._extra_entry("Donner SVG Editor.app/Contents/Resources/extra", mode)
                with self.assertRaises(ValueError):
                    self.native.inspect_archive(path)

    def test_entry_and_aggregate_limits_apply_before_member_reads(self):
        from unittest import mock
        self.package()
        path = self.output / self.native.ARCHIVE
        for key, limit in (("MAX_FILES", 1), ("MAX_MEMBER_BYTES", 1), ("MAX_TOTAL_BYTES", 1)):
            with self.subTest(key=key), mock.patch.object(self.native, key, limit), \
                 mock.patch("zipfile.ZipFile.open", side_effect=AssertionError("must not read")):
                with self.assertRaises(ValueError):
                    self.native.inspect_archive(path)

    def test_native_dependency_resolution_stays_in_bundle(self):
        from unittest import mock
        image = self.app / "Contents/MacOS/Donner"
        library = self.app / "Contents/Frameworks/libfixture.dylib"
        library.parent.mkdir()
        library.write_bytes(b"signed")
        def output(*args):
            if "-l" in args:
                return "cmd LC_RPATH\npath @executable_path/../Frameworks (offset 12)\n"
            return str(image) + ":\n\t@rpath/libfixture.dylib (compatibility version 1.0.0)\n"
        with mock.patch.object(self.native, "_tool", side_effect=output):
            self.native._check_dependencies(image, self.app, [])
        evil = self.app / "Contents/Resources/evil.dylib"
        evil.write_bytes(b"not admitted code")
        for dependency in ("/opt/fixture/lib.dylib", "@loader_path/../../../escape.dylib",
                           "@rpath/missing.dylib", "@rpath/../Resources/evil.dylib",
                           "@executable_path/../Resources/evil.dylib",
                           "/usr/lib/../../opt/fixture/lib.dylib"):
            with self.subTest(dependency=dependency), mock.patch.object(
                    self.native, "_tool", return_value=str(image) + ":\n\t" + dependency):
                with self.assertRaises(ValueError):
                    self.native._check_dependencies(image, self.app, [library.parent])

    def test_entrypoint_probe_clears_library_and_checkout_borrowing(self):
        import os
        import subprocess
        from unittest import mock
        def execute(command, **options):
            self.assertEqual(command[1], "--donner-release-loader-check")
            self.assertEqual(options["cwd"], self.app.parent)
            for key in ("BUILD_WORKING_DIRECTORY", "RUNFILES_DIR", "DYLD_LIBRARY_PATH",
                        "LD_LIBRARY_PATH", "RUNFILES_MANIFEST_FILE"):
                self.assertNotIn(key, options["env"])
            options["stderr"].write(b"Unknown option --donner-release-loader-check\n"
                                    b"Usage: donner-editor [filename]\n")
            return subprocess.CompletedProcess(command, 1)
        poisoned = {key: "fixture" for key in ("BUILD_WORKING_DIRECTORY", "RUNFILES_DIR",
                    "DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH", "RUNFILES_MANIFEST_FILE")}
        with mock.patch.dict(os.environ, poisoned), \
             mock.patch.object(self.native.subprocess, "run", side_effect=execute):
            self.native._check_entrypoint(self.app)

    def test_xcode_drift_or_missing_native_role_is_refused(self):
        self.package()
        import json
        path = self.output / self.native.NAMES[2]
        provenance = json.loads(path.read_text())
        provenance["toolchain"]["xcode"] = "another Xcode"
        path.write_text(json.dumps(provenance))
        with self.assertRaisesRegex(ValueError, "toolchain"):
            self.verify()
        with self.assertRaises(ValueError):
            release_candidate._verify_role_files("editor-macos", set(), "0.8.0")



if __name__ == "__main__":
    unittest.main()
