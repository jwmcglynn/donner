#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


_CHECKER_PATH = Path(__file__).with_name("check_banned_patterns.py")
_CHECKER_SPEC = importlib.util.spec_from_file_location("check_banned_patterns", _CHECKER_PATH)
assert _CHECKER_SPEC is not None
assert _CHECKER_SPEC.loader is not None
check_banned_patterns = importlib.util.module_from_spec(_CHECKER_SPEC)
_CHECKER_SPEC.loader.exec_module(check_banned_patterns)


class CheckBannedPatternsTests(unittest.TestCase):
    def _write_source(self, source: str, filename: str = "Example.cc") -> Path:
        temp_dir = Path(tempfile.mkdtemp())
        source_path = temp_dir / "donner" / "base" / filename
        source_path.parent.mkdir(parents=True)
        source_path.write_text(source, encoding="utf-8")
        return source_path

    def _descriptions_for(self, source: str, filename: str = "Example.cc") -> list[str]:
        return [
            error[1]
            for error in check_banned_patterns.check_file(self._write_source(source, filename))
        ]

    def test_blocks_typographic_hyphens_and_dashes_in_comments_and_strings(self):
        descriptions = self._descriptions_for(
            '// A comment with a non-breaking hyphen: \u2011\n'
            '// A comment with an em dash: \u2014\n'
            'const char* range = "10\u201320";\n'
        )

        self.assertTrue(any("non-breaking hyphen (U+2011" in desc for desc in descriptions))
        self.assertTrue(any("em dash (U+2014 EM DASH)" in desc for desc in descriptions))
        self.assertTrue(any("en dash (U+2013 EN DASH)" in desc for desc in descriptions))

    def test_blocks_smart_quotes(self):
        descriptions = self._descriptions_for(
            '// Smart single quotes: \u2018value\u2019\n'
            'const char* text = "\u201cvalue\u201d";\n'
        )

        self.assertEqual(4, len([desc for desc in descriptions if "smart quote" in desc]))

    def test_blocks_hidden_unicode_whitespace(self):
        descriptions = self._descriptions_for(
            "int before\u00a0= 1;\n"
            "int after\u200b= 2;\n"
        )

        self.assertTrue(any("U+00A0 NO-BREAK SPACE" in desc for desc in descriptions))
        self.assertTrue(any("U+200B ZERO WIDTH SPACE" in desc for desc in descriptions))

    def test_blocks_raw_unicode_in_script_sources_without_cpp_rules(self):
        descriptions = self._descriptions_for(
            "# Python comment with a smart quote: \u2019\n"
            "text = 'long long in a Python string should not trigger the C++ rule'\n",
            filename="formatter.py",
        )

        self.assertEqual(1, len(descriptions))
        self.assertIn("smart quote", descriptions[0])

    def test_allows_other_intentional_unicode_literals(self):
        descriptions = self._descriptions_for(
            'const char* arrow = "\u2192";\n'
            'const char* star = "\u2731";\n'
            'const char* multiply = "\u00d7";\n'
        )

        self.assertEqual([], descriptions)


if __name__ == "__main__":
    unittest.main()
