#!/usr/bin/env python3
"""Integrity test for the embedded Google Fonts pin table (Design 0013 W3).

Validates the generated manifest (from GOOGLE_FONTS in fonts.bzl) without any
network access: every family must be commit-pinned to a single google/fonts
revision, carry a well-formed sha256, and the curated set must keep spanning
sans / serif / mono / display so the picker always has variety.
"""
import json
import re
import sys
import unittest

_MANIFEST_PATH = sys.argv[1] if len(sys.argv) > 1 else None

_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_VAR = re.compile(r"^kGF[A-Za-z0-9]+Ttf$")
_REPO = re.compile(r"^gfont_[a-z0-9_]+$")
_ALLOWED_CATEGORIES = {"SansSerif", "Serif", "Monospace", "Display", "Handwriting"}


def _load():
    with open(_MANIFEST_PATH, encoding="utf-8") as handle:
        return json.load(handle)


class GoogleFontsIntegrityTest(unittest.TestCase):
    def setUp(self):
        self.manifest = _load()
        self.commit = self.manifest["commit"]
        self.fonts = self.manifest["fonts"]

    def test_commit_is_pinned_sha(self):
        self.assertRegex(self.commit, _HEX40)

    def test_curated_set_size(self):
        # Design 0013 W3 curates 8-12 families.
        self.assertGreaterEqual(len(self.fonts), 8)
        self.assertLessEqual(len(self.fonts), 12)

    def test_entries_are_wellformed(self):
        for font in self.fonts:
            with self.subTest(family=font["family"]):
                self.assertTrue(font["family"].strip())
                self.assertIn(font["category"], _ALLOWED_CATEGORIES)
                self.assertRegex(font["var"], _VAR)
                self.assertRegex(font["repo"], _REPO)
                self.assertRegex(font["sha256"], _HEX64)
                self.assertGreater(font["bytes"], 0)
                self.assertTrue(font["file"])

    def test_urls_are_commit_pinned_and_https(self):
        prefix = "https://raw.githubusercontent.com/google/fonts/" + self.commit + "/"
        for font in self.fonts:
            with self.subTest(family=font["family"]):
                self.assertTrue(
                    font["url"].startswith(prefix),
                    msg="URL not pinned to the manifest commit: " + font["url"],
                )

    def test_identifiers_are_unique(self):
        for key in ("family", "var", "repo", "sha256"):
            values = [font[key] for font in self.fonts]
            self.assertEqual(len(values), len(set(values)), msg="duplicate " + key)

    def test_set_spans_categories(self):
        categories = {font["category"] for font in self.fonts}
        for required in ("SansSerif", "Serif", "Monospace", "Display"):
            self.assertIn(required, categories)


if __name__ == "__main__":
    # Strip the manifest path arg so unittest does not treat it as a test name.
    argv = [sys.argv[0]] + sys.argv[2:]
    unittest.main(argv=argv)
