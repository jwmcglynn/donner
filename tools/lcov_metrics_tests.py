#!/usr/bin/env python3
"""Tests for lcov_metrics.py."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lcov_metrics


class LcovMetricsTest(unittest.TestCase):
    def test_collects_line_and_branch_counts_from_brda_entries(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,0
BRDA:10,0,0,1
BRDA:10,0,1,0
BRDA:11,0,0,-
end_of_record
SF:donner/base/Bar.cc
DA:5,3
BRDA:5,0,0,2
end_of_record
"""
        )

        metrics = lcov_metrics.collect_lcov_metrics(report)

        self.assertEqual(metrics.lines.hit, 2)
        self.assertEqual(metrics.lines.found, 3)
        self.assertEqual(metrics.branches.hit, 2)
        self.assertEqual(metrics.branches.found, 4)
        self.assertEqual(metrics.branches.hits_needed_for(75.0), 1)
        self.assertEqual(metrics.files[0].missed_branches, 2)

    def test_json_summary_reports_branch_target_shortfall_and_top_misses(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
BRDA:10,0,0,1
BRDA:10,0,1,0
end_of_record
SF:donner/base/Bar.cc
DA:5,0
BRDA:5,0,0,0
BRDA:5,0,1,0
end_of_record
"""
        )

        summary = lcov_metrics.metrics_to_json(
            lcov_metrics.collect_lcov_metrics(report),
            branch_target=85.0,
            top_misses=1,
        )

        self.assertEqual(summary["source_files"], 2)
        self.assertEqual(summary["branches"]["hit"], 1)
        self.assertEqual(summary["branches"]["found"], 4)
        self.assertEqual(summary["branches"]["hits_needed_for_target"], 3)
        self.assertEqual(len(summary["top_branch_misses"]), 1)
        self.assertEqual(summary["top_branch_misses"][0]["source_file"], "donner/base/Bar.cc")

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
