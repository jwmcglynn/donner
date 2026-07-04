#!/usr/bin/env python3
"""Tests for check_lcov_report.py."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_lcov_report


class CheckLcovReportTest(unittest.TestCase):
    def test_accepts_report_with_executable_lines(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,0
LF:2
LH:1
end_of_record
"""
        )

        stats = check_lcov_report.validate_lcov_report(report)

        self.assertEqual(stats.source_files, 1)
        self.assertEqual(stats.line_entries, 2)
        self.assertEqual(stats.found_lines, 2)
        self.assertEqual(stats.hit_lines, 1)

    def test_rejects_report_empty_after_bazel_coverage(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
FNF:0
FNH:0
LF:0
LH:0
end_of_record
"""
        )

        with self.assertRaisesRegex(ValueError, "no executable line data"):
            check_lcov_report.validate_lcov_report(report)

    def test_rejects_report_without_sources(self) -> None:
        report = self._write_report("")

        with self.assertRaisesRegex(ValueError, "no executable line data"):
            check_lcov_report.validate_lcov_report(report)

    def _write_report(self, contents: str) -> Path:
        report = Path(self._tmpdir.name) / "report.dat"
        report.write_text(contents, encoding="utf-8")
        return report

    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmpdir.cleanup()


if __name__ == "__main__":
    unittest.main()
