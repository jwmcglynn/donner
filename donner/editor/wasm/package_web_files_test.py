"""Regression coverage for immutable font bundle paths and collision rejection."""

from pathlib import Path
import tempfile
import unittest

from package_web_files import package_files


class PackageFilesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "output"

    def file(self, relative, content=b"fixture"):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_assets_keep_content_addressed_subpaths_and_notices(self):
        index = self.file("app/index.html", b"application")
        font = self.file("assets/fonts/0123.woff2", b"wOF2fixture")
        notice = self.file("assets/notices/fonts.txt", b"license")
        package_files(self.output, [index], [self.root / "assets"])
        self.assertEqual((self.output / "index.html").read_bytes(), index.read_bytes())
        self.assertEqual((self.output / "fonts/0123.woff2").read_bytes(), font.read_bytes())
        self.assertEqual((self.output / "notices/fonts.txt").read_bytes(), notice.read_bytes())

    def test_duplicate_flat_names_fail_before_writing(self):
        first = self.file("a/app.js")
        second = self.file("b/app.js")
        with self.assertRaisesRegex(ValueError, "Duplicate web package path: app.js"):
            package_files(self.output, [first, second], [])
        self.assertFalse(self.output.exists())

    def test_asset_tree_cannot_overwrite_bootstrap(self):
        bootstrap = self.file("app/editor-bootstrap.js")
        self.file("assets/editor-bootstrap.js")
        with self.assertRaisesRegex(ValueError, "Duplicate web package path"):
            package_files(self.output, [bootstrap], [self.root / "assets"])
        self.assertFalse(self.output.exists())

    def test_directory_and_file_collision_fails_before_writing(self):
        file = self.file("app/fonts")
        self.file("assets/fonts/a.woff2")
        with self.assertRaisesRegex(ValueError, "File/directory collision"):
            package_files(self.output, [file], [self.root / "assets"])
        self.assertFalse(self.output.exists())

    def test_asset_symlinks_cannot_escape_the_tree(self):
        target = self.file("outside/font.woff2")
        tree = self.root / "assets"
        tree.mkdir()
        (tree / "link.woff2").symlink_to(target)
        with self.assertRaisesRegex(ValueError, "Symlink in web package asset tree"):
            package_files(self.output, [], [tree])
        self.assertFalse(self.output.exists())

    def test_ambiguous_path_names_are_rejected(self):
        for name in ["line\nfeed.woff2", "back\\slash.woff2"]:
            source = self.file(f"assets/{name}")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid web package path"):
                package_files(self.output, [source], [])

    def test_existing_output_cannot_redirect_writes_through_a_symlink(self):
        self.file("assets/fonts/a.woff2")
        outside = self.root / "outside"
        outside.mkdir()
        self.output.mkdir()
        (self.output / "fonts").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "output must be empty"):
            package_files(self.output, [], [self.root / "assets"])
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
