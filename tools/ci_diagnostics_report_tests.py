#!/usr/bin/env python3
"""Tests for ci_diagnostics_report.py."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_diagnostics_report


class CiDiagnosticsReportTests(unittest.TestCase):
    def test_console_tail_is_reported_for_incomplete_bep(self) -> None:
        bep = self._write("build/bep.json", json.dumps({"id": {"started": {}}}) + "\n")
        console_lines = [f"line-{index}" for index in range(1, 86)]
        console = self._write("build/console.log", "\n".join(console_lines) + "\n")

        lines: list[str] = []
        ci_diagnostics_report.console_tail_summary("Build", bep, console, lines)

        report = "\n".join(lines)
        self.assertIn("Build console tail", report)
        self.assertIn("line-85", report)
        self.assertIn("line-6", report)
        self.assertNotIn("line-5", lines)

    def test_console_tail_is_suppressed_for_complete_bep(self) -> None:
        bep = self._write("build/bep.json", json.dumps({"id": {"buildFinished": {}}}) + "\n")
        console = self._write("build/console.log", "last visible line\n")

        lines: list[str] = []
        ci_diagnostics_report.console_tail_summary("Build", bep, console, lines)

        self.assertEqual(lines, [])

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
