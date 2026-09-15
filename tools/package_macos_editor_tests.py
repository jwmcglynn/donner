"""Checks the portable archive and dependency boundaries of app assembly."""
from pathlib import Path
import tempfile
import unittest
import zipfile

from tools.package_macos_editor import archive_app, load_paths, system_library


class AppArchiveTests(unittest.TestCase):
    def test_archive_preserves_executables_and_relative_instruction_links(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "Donner SVG Editor.app"
            resources = app / "Contents/Resources"
            resources.mkdir(parents=True)
            binary = app / "Contents/MacOS/DonnerSVGEditor"
            binary.parent.mkdir()
            binary.write_bytes(b"executable input")
            binary.chmod(0o755)
            (resources / "AGENTS.md").write_text("Read the bundled skill.\n")
            (resources / "instructions.md").symlink_to("AGENTS.md")
            first, second = root / "first.zip", root / "second.zip"
            archive_app(app, first)
            archive_app(app, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                prefix = "Donner SVG Editor.app/Contents/"
                executable = archive.getinfo(prefix + "MacOS/DonnerSVGEditor")
                self.assertEqual(executable.external_attr >> 16, 0o100755)
                link = archive.getinfo(prefix + "Resources/instructions.md")
                self.assertEqual(link.external_attr >> 16, 0o120777)
                self.assertEqual(archive.read(link), b"AGENTS.md")
                self.assertTrue(all(info.date_time == (1980, 1, 1, 0, 0, 0)
                                    for info in archive.infolist()))

    def test_archive_rejects_escaping_and_absolute_symlinks(self):
        for target in ("../../secret", "/etc/passwd"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                app = root / "Editor.app"
                app.mkdir()
                (app / "leak").symlink_to(target)
                with self.assertRaisesRegex(ValueError, "symlink escapes"):
                    archive_app(app, root / "bad.zip")

    def test_archive_rejects_directory_symlinks_outside_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "Editor.app"
            app.mkdir()
            (app / "leak").symlink_to(root, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "symlink escapes"):
                archive_app(app, root / "bad.zip")


class MachOLoadTests(unittest.TestCase):
    def test_dependency_paths_do_not_include_library_install_id(self):
        commands = """Load command 0
          cmd LC_ID_DYLIB
      cmdsize 64
         name @rpath/libexample.dylib (offset 24)
Load command 1
          cmd LC_LOAD_DYLIB
      cmdsize 64
         name /usr/lib/libSystem.B.dylib (offset 24)
Load command 2
          cmd LC_LOAD_WEAK_DYLIB
      cmdsize 80
         name /build with spaces/liboptional.dylib (offset 24)
Load command 3
          cmd LC_RPATH
      cmdsize 48
         path @executable_path/runfiles (offset 12)
"""
        self.assertEqual(load_paths(commands, {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB"}),
                         ["/usr/lib/libSystem.B.dylib", "/build with spaces/liboptional.dylib"])
        self.assertEqual(load_paths(commands, {"LC_RPATH"}), ["@executable_path/runfiles"])

    def test_non_system_dependencies_must_be_bundled(self):
        for path in ("/usr/lib/libSystem.B.dylib", "/System/Library/Frameworks/AppKit.framework/AppKit"):
            self.assertTrue(system_library(path))
        for path in ("/opt/homebrew/lib/libexample.dylib", "/usr/library/libexample.dylib", "@rpath/libexample.dylib"):
            self.assertFalse(system_library(path))


if __name__ == "__main__":
    unittest.main()
