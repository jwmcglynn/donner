#!/usr/bin/env python3
"""Tests for ci_diagnostics_report.py."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_diagnostics_report


class CiDiagnosticsReportTests(unittest.TestCase):
    def test_console_tail_is_reported_for_incomplete_bep(self) -> None:
        bep = self._write("build/bep.json", json.dumps({"id": {"started": {}}}) + "\n")
        console_lines = [f"line-{index}" for index in range(1, 86)]
        console = self._write("build/console.log", "\n".join(console_lines) + "\n")

        lines: list[str] = []
        ci_diagnostics_report.console_tail_summary("Build", bep, console, lines, include_raw=True)

        report = "\n".join(lines)
        self.assertIn("Build console tail", report)
        self.assertIn("line-85", report)
        self.assertIn("line-6", report)
        self.assertNotIn("line-5", lines)

    def test_console_tail_is_suppressed_for_complete_bep(self) -> None:
        bep = self._write("build/bep.json", json.dumps({"id": {"buildFinished": {}}}) + "\n")
        console = self._write("build/console.log", "last visible line\n")

        lines: list[str] = []
        ci_diagnostics_report.console_tail_summary("Build", bep, console, lines, include_raw=True)

        self.assertEqual(lines, [])

    def test_default_report_does_not_publish_raw_configuration(self) -> None:
        self._write("build/bep.json", json.dumps({"id": {"started": {}}}) + "\n")
        self._write("build/console.log", "--remote_cache=https://cache.example.invalid/fixture\n"
                    "--remote_header=Authorization=Bearer test-only-token\n")
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["ci_diagnostics_report.py", self._tmpdir.name]), \
                redirect_stdout(output):
            self.assertEqual(ci_diagnostics_report.main(), 0)
        self.assertNotIn("cache.example.invalid", output.getvalue())
        self.assertNotIn("test-only-token", output.getvalue())
        self.assertIn("omitted", output.getvalue())

    def test_raw_console_tails_require_an_explicit_local_option(self) -> None:
        self._write("build/bep.json", json.dumps({"id": {"started": {}}}) + "\n")
        self._write("build/console.log", "local-debug-sentinel\n")
        output = io.StringIO()
        with mock.patch.object(sys, "argv", ["ci_diagnostics_report.py", self._tmpdir.name,
                                           "--include-console-tails"]), redirect_stdout(output):
            self.assertEqual(ci_diagnostics_report.main(), 0)
        self.assertIn("local-debug-sentinel", output.getvalue())

    def _write(self, relative_path: str, contents: str) -> str:
        path = Path(self._tmpdir.name) / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return str(path)

    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmpdir.cleanup()


if __name__ == "__main__":
    unittest.main()
