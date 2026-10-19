"""Pins how the repository's clang-format lays out constructs that changed between releases."""

import subprocess
import sys
import unittest

from tools import test_timing

CLANG_FORMAT, STYLE, INPUT, EXPECTED = sys.argv[1:5]


def _format(path):
    with open(path, encoding="utf-8") as source:
        return subprocess.run(
            [CLANG_FORMAT, f"--style=file:{STYLE}", "--assume-filename=fixture.cc"],
            stdin=source, capture_output=True, text=True, check=True,
            timeout=test_timing.STALL_GUARD_SECONDS).stdout


class ClangFormatFixtureTest(unittest.TestCase):
    def test_version_is_the_pinned_release(self):
        version = subprocess.run([CLANG_FORMAT, "--version"], capture_output=True, text=True,
                                 check=True, timeout=test_timing.STALL_GUARD_SECONDS).stdout
        self.assertIn("clang-format version 21.1.6", version)

    def test_divergent_constructs_format_as_committed(self):
        with open(EXPECTED, encoding="utf-8") as expected:
            self.assertEqual(_format(INPUT), expected.read())

    def test_committed_layout_is_stable(self):
        with open(EXPECTED, encoding="utf-8") as expected:
            self.assertEqual(_format(EXPECTED), expected.read())


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
