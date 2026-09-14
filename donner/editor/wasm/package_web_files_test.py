"""Regression coverage for immutable font bundle paths and collision rejection."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from package_web_files import main, package_files


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
        package_files(self.output, [index], [
            ("fonts/0123.woff2", font), ("notices/fonts.txt", notice),
        ])
        self.assertEqual((self.output / "index.html").read_bytes(), index.read_bytes())
        self.assertEqual((self.output / "fonts/0123.woff2").read_bytes(), font.read_bytes())
        self.assertEqual((self.output / "notices/fonts.txt").read_bytes(), notice.read_bytes())

    def test_declared_asset_symlink_is_materialized_as_a_regular_file(self):
        target = self.file("bazel-output/actual.woff2", b"declared font")
        link = self.root / "sandbox/font.woff2"
        link.parent.mkdir()
        link.symlink_to(target)
        package_files(self.output, [], [("fonts/font.woff2", link)])
        materialized = self.output / "fonts/font.woff2"
        self.assertFalse(materialized.is_symlink())
        self.assertEqual(materialized.read_bytes(), b"declared font")

    def test_unlisted_neighbors_are_never_discovered_or_copied(self):
        declared = self.file("tree/fonts/declared.woff2")
        self.file("tree/fonts/unlisted.woff2", b"not declared")
        outside = self.file("outside/unlisted.txt")
        (declared.parent / "unlisted-link").symlink_to(outside.parent, target_is_directory=True)
        package_files(self.output, [], [("fonts/declared.woff2", declared)])
        self.assertEqual(
            sorted(path.relative_to(self.output).as_posix() for path in self.output.rglob("*")),
            ["fonts", "fonts/declared.woff2"],
        )

    def test_duplicate_flat_names_fail_before_writing(self):
        first = self.file("a/app.js")
        second = self.file("b/app.js")
        with self.assertRaisesRegex(ValueError, "Duplicate web package path: app.js"):
            package_files(self.output, [first, second], [])
        self.assertFalse(self.output.exists())

    def test_asset_cannot_overwrite_bootstrap(self):
        bootstrap = self.file("app/editor-bootstrap.js")
        asset = self.file("assets/editor-bootstrap.js")
        with self.assertRaisesRegex(ValueError, "Duplicate web package path"):
            package_files(self.output, [bootstrap], [("editor-bootstrap.js", asset)])
        self.assertFalse(self.output.exists())

    def test_directory_and_file_collision_fails_before_writing(self):
        file = self.file("app/fonts")
        asset = self.file("assets/fonts/a.woff2")
        with self.assertRaisesRegex(ValueError, "File/directory collision"):
            package_files(self.output, [file], [("fonts/a.woff2", asset)])
        self.assertFalse(self.output.exists())

    def test_directory_symlinks_are_not_asset_files(self):
        target = self.file("outside/font.woff2")
        link = self.root / "directory-link"
        link.symlink_to(target.parent, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "input is not a file"):
            package_files(self.output, [], [("fonts", link)])
        self.assertFalse(self.output.exists())

    def test_explicit_asset_destinations_cannot_escape_or_alias(self):
        source = self.file("font.woff2")
        for relative in ["", ".", "../escape", "/absolute", "fonts/../a", "fonts//a", "./a", "a/"]:
            with self.subTest(relative=relative), self.assertRaisesRegex(ValueError, "Invalid web package path"):
                package_files(self.output, [], [(relative, source)])
            self.assertFalse(self.output.exists())

    def test_duplicate_asset_destinations_fail_before_writing(self):
        first = self.file("first.woff2")
        second = self.file("second.woff2")
        with self.assertRaisesRegex(ValueError, "Duplicate web package path"):
            package_files(self.output, [], [("fonts/a.woff2", first), ("fonts/a.woff2", second)])
        self.assertFalse(self.output.exists())

    def test_asset_arguments_preserve_the_explicit_destination(self):
        source = self.file("input.woff2")
        with patch("sys.argv", ["package_web_files", "--output", str(self.output),
                               "--asset", "fonts/declared.woff2", str(source)]):
            main()
        self.assertEqual((self.output / "fonts/declared.woff2").read_bytes(), source.read_bytes())

    def test_ambiguous_path_names_are_rejected(self):
        for name in ["line\nfeed.woff2", "back\\slash.woff2"]:
            source = self.file(f"assets/{name}")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid web package path"):
                package_files(self.output, [source], [])

    def test_existing_output_cannot_redirect_writes_through_a_symlink(self):
        asset = self.file("assets/fonts/a.woff2")
        outside = self.root / "outside"
        outside.mkdir()
        self.output.mkdir()
        (self.output / "fonts").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "output must be empty"):
            package_files(self.output, [], [("fonts/a.woff2", asset)])
        self.assertEqual(list(outside.iterdir()), [])

    def test_output_root_symlink_is_rejected(self):
        source = self.file("font.woff2")
        outside = self.root / "outside"
        outside.mkdir()
        self.output.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "output must not be a symlink"):
            package_files(self.output, [], [("fonts/a.woff2", source)])
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
