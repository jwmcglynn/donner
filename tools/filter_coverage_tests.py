#!/usr/bin/env python3
"""Tests for filter_coverage.py."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class FilterCoverageCliTest(unittest.TestCase):
    def test_default_mode_filters_without_console_output(self) -> None:
        source = self._write_source()
        input_report = self._write_report(source)
        output_report = Path(self._tmpdir.name) / "filtered.dat"

        result = self._run_filter(input_report, output_report)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")
        self.assertEqual(
            output_report.read_text(encoding="utf-8"),
            f"""\
SF:{source}
DA:1,1
LF:1
LH:1
end_of_record
""",
        )

    def test_verbose_mode_reports_processed_and_excluded_lines(self) -> None:
        source = self._write_source()
        input_report = self._write_report(source)
        output_report = Path(self._tmpdir.name) / "filtered.dat"

        result = self._run_filter(input_report, output_report, "--verbose")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Processing source file: {source}", result.stdout)
        self.assertIn("Excluding line: DA:2,1", result.stdout)
        self.assertEqual(result.stderr, "")

    def _write_source(self) -> Path:
        source = Path(self._tmpdir.name) / "Source.cc"
        source.write_text(
            """\
int covered() { return 1; }
int excluded() { return 2; }  // LCOV_EXCL_LINE
""",
            encoding="utf-8",
        )
        return source

    def _write_report(self, source: Path) -> Path:
        report = Path(self._tmpdir.name) / "coverage.dat"
        report.write_text(
            f"""\
SF:{source}
DA:1,1
DA:2,1
LF:2
LH:2
end_of_record
""",
            encoding="utf-8",
        )
        return report

    def _run_filter(
        self, input_report: Path, output_report: Path, *extra_args: str
    ) -> subprocess.CompletedProcess[str]:
        script = Path(__file__).with_name("filter_coverage.py")
        return subprocess.run(
            [
                sys.executable,
                str(script),
                "--input",
                str(input_report),
                "--output",
                str(output_report),
                *extra_args,
            ],
            check=False,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmpdir.cleanup()


if __name__ == "__main__":
    unittest.main()
