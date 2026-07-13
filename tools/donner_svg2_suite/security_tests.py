#!/usr/bin/env python3
"""Security tests for the SVG2 suite path-safety layer.

Covers the traversal, symlink-escape, absolute-path, URL, and size-cap subset
of design 0057's planned security_tests target. Inputs are treated as hostile:
each check must reject before any file outside the corpus root is opened.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest_validation
import path_safety


class EnsureRelativeTest(unittest.TestCase):
    def assert_rejected(self, raw: str):
        with self.assertRaises(path_safety.UnsafePathError):
            path_safety.ensure_relative(raw)

    def test_absolute_path_rejected(self):
        self.assert_rejected("/etc/passwd")

    def test_parent_traversal_rejected(self):
        self.assert_rejected("../secret")
        self.assert_rejected("tests/../../secret")

    def test_url_schemes_rejected(self):
        for raw in ("http://x/y", "https://x/y", "file:///etc/passwd", "data:text/plain,hi", "C:/windows"):
            with self.subTest(raw=raw):
                self.assert_rejected(raw)

    def test_backslash_rejected(self):
        self.assert_rejected("tests\\evil")

    def test_empty_and_nul_rejected(self):
        self.assert_rejected("")
        self.assert_rejected("tests/\x00evil")

    def test_current_directory_component_rejected(self):
        self.assert_rejected("tests/./evil")

    def test_safe_relative_path_accepted(self):
        self.assertEqual(str(path_safety.ensure_relative("tests/a/b.svg")), "tests/a/b.svg")


class ResolveWithinRootTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_symlink_component_rejected(self):
        outside = self.root.parent / "outside_target"
        outside.mkdir(exist_ok=True)
        try:
            link = self.root / "link"
            link.symlink_to(outside, target_is_directory=True)
            with self.assertRaises(path_safety.UnsafePathError):
                path_safety.resolve_within_root(self.root, "link/secret.svg")
        finally:
            if outside.exists():
                outside.rmdir()

    def test_symlink_escape_to_file_rejected(self):
        secret = self.root.parent / "secret.txt"
        secret.write_text("classified", encoding="utf-8")
        try:
            link = self.root / "passwd"
            link.symlink_to(secret)
            with self.assertRaises(path_safety.UnsafePathError):
                path_safety.resolve_within_root(self.root, "passwd")
        finally:
            secret.unlink(missing_ok=True)

    def test_safe_path_resolves(self):
        (self.root / "tests").mkdir()
        (self.root / "tests" / "a.svg").write_text("x", encoding="utf-8")
        resolved = path_safety.resolve_within_root(self.root, "tests/a.svg")
        self.assertTrue(resolved.endswith(os.path.join("tests", "a.svg")))


class ReadTextCappedTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_oversize_file_rejected(self):
        path = self.root / "big.json"
        path.write_bytes(b"a" * 2048)
        with self.assertRaises(path_safety.UnsafePathError):
            path_safety.read_text_capped(path, max_bytes=1024)

    def test_within_cap_read(self):
        path = self.root / "small.json"
        path.write_text("hello", encoding="utf-8")
        self.assertEqual(path_safety.read_text_capped(path, max_bytes=1024), "hello")


class HostileManifestTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_manifest_symlink_escape_rejected(self):
        secret = self.root.parent / "manifest_secret.svg"
        secret.write_text("classified", encoding="utf-8")
        try:
            tests_dir = self.root / "tests" / "painting" / "paint-order"
            tests_dir.mkdir(parents=True)
            (tests_dir / "tspan-boundary.png").write_bytes(b"x")
            (self.root / "fonts").mkdir()
            (self.root / "fonts" / "NotoSans-Regular.ttf").write_bytes(b"x")
            (tests_dir / "tspan-boundary.svg").symlink_to(secret)

            manifest = {
                "schema": "https://donner.graphics/svg2-suite/corpus-v1.schema.json",
                "corpus": "donner-svg2",
                "revision": "0" * 40,
                "tests": [
                    {
                        "id": "donner-svg2/painting/paint-order/tspan-boundary",
                        "input": "tests/painting/paint-order/tspan-boundary.svg",
                        "oracle": {
                            "kind": "png",
                            "path": "tests/painting/paint-order/tspan-boundary.png",
                            "width": 500,
                            "height": 500,
                            "provenance": "independently-reviewed-reference",
                        },
                        "assertion": "paint-order does not split shaping across a paint-only tspan",
                        "spec_requirements": ["svg2-cr-20181004/painting/paint-order/req-03"],
                        "capabilities": ["text"],
                        "resources": ["fonts/NotoSans-Regular.ttf"],
                    }
                ],
            }
            path = self.root / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            errors = manifest_validation.validate_manifest(path)
            self.assertTrue(any("symlink" in error for error in errors), errors)
        finally:
            secret.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
