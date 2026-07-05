#!/usr/bin/env python3
"""Tests for lcov_metrics.py."""

from pathlib import Path
import json
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
        self.assertEqual(metrics.codecov_lines.hits, 1)
        self.assertEqual(metrics.codecov_lines.misses, 1)
        self.assertEqual(metrics.codecov_lines.partials, 1)
        self.assertEqual(metrics.codecov_lines.total, 3)
        self.assertEqual(metrics.branches.hit, 2)
        self.assertEqual(metrics.branches.found, 4)
        self.assertEqual(metrics.branches.hits_needed_for(75.0), 1)
        self.assertEqual(metrics.files[0].missed_branches, 2)

    def test_codecov_line_counts_treat_partially_covered_branch_lines_as_partials(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,3
DA:12,0,optional-checksum
DA:13,2
BRDA:11,0,0,1
BRDA:11,0,1,0
BRDA:13,0,0,4
BRDA:13,0,1,2
end_of_record
"""
        )

        metrics = lcov_metrics.collect_lcov_metrics(report)

        self.assertEqual(metrics.lines.hit, 3)
        self.assertEqual(metrics.lines.found, 4)
        self.assertEqual(metrics.codecov_lines.hits, 2)
        self.assertEqual(metrics.codecov_lines.misses, 1)
        self.assertEqual(metrics.codecov_lines.partials, 1)
        self.assertEqual(metrics.codecov_lines.total, 4)
        self.assertEqual(metrics.codecov_lines.display_percent, 50)
        self.assertEqual(metrics.codecov_lines.hits_needed_for(75.0), 1)

    def test_codecov_display_percent_rounds_exact_percent_to_whole_number(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:1,1
DA:2,1
DA:3,1
DA:4,1
DA:5,1
DA:6,1
DA:7,1
DA:8,0
end_of_record
"""
        )

        metrics = lcov_metrics.collect_lcov_metrics(report)

        self.assertAlmostEqual(metrics.codecov_lines.percent, 87.5)
        self.assertEqual(metrics.codecov_lines.display_percent, 88)

    def test_json_summary_reports_branch_target_shortfall_and_top_misses(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,2
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
            coverage_target=90.0,
            branch_target=85.0,
            top_misses=1,
        )

        self.assertEqual(summary["source_files"], 2)
        self.assertEqual(summary["codecov_lines"]["hits"], 1)
        self.assertEqual(summary["codecov_lines"]["misses"], 1)
        self.assertEqual(summary["codecov_lines"]["partials"], 1)
        self.assertEqual(summary["codecov_lines"]["total"], 3)
        self.assertEqual(summary["codecov_lines"]["display_percent"], 33)
        self.assertEqual(summary["codecov_lines"]["hits_needed_for_target"], 2)
        self.assertEqual(summary["branches"]["hit"], 1)
        self.assertEqual(summary["branches"]["found"], 4)
        self.assertEqual(summary["branches"]["hits_needed_for_target"], 3)
        self.assertEqual(len(summary["top_branch_misses"]), 1)
        self.assertEqual(summary["top_branch_misses"][0]["source_file"], "donner/base/Bar.cc")
        self.assertEqual(len(summary["top_codecov_partials"]), 1)
        self.assertEqual(summary["top_codecov_partials"][0]["source_file"], "donner/base/Foo.cc")

    def test_codecov_reference_json_limits_metrics_to_processed_line_universe(self) -> None:
        report = self._write_report(
            """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,3
DA:12,1
BRDA:11,0,0,1
BRDA:11,0,1,0
end_of_record
SF:donner/base/Dropped.cc
DA:1,1
end_of_record
"""
        )
        reference_json = self._write_json(
            {
                "report": {
                    "files": [
                        {
                            "name": "donner/base/Foo.cc",
                            "line_coverage": [[10, 0], [11, 2], [13, 1]],
                        },
                        {
                            "name": "donner/base/Missing.cc",
                            "line_coverage": [[20, 1]],
                        },
                    ]
                }
            }
        )

        metrics = lcov_metrics.collect_lcov_metrics(
            report, reference_lines=lcov_metrics.load_codecov_reference_lines(reference_json)
        )

        self.assertEqual(metrics.lines.hit, 2)
        self.assertEqual(metrics.lines.found, 4)
        self.assertEqual(metrics.codecov_lines.hits, 1)
        self.assertEqual(metrics.codecov_lines.misses, 2)
        self.assertEqual(metrics.codecov_lines.partials, 1)
        self.assertEqual(metrics.codecov_lines.total, 4)
        self.assertEqual(metrics.branches.hit, 1)
        self.assertEqual(metrics.branches.found, 2)
        self.assertEqual([file.source_file for file in metrics.files],
                         ["donner/base/Foo.cc", "donner/base/Missing.cc"])

    def _write_report(self, contents: str) -> Path:
        report = Path(self._tmpdir.name) / "report.dat"
        report.write_text(contents, encoding="utf-8")
        return report

    def _write_json(self, contents: object) -> Path:
        report = Path(self._tmpdir.name) / "codecov.json"
        report.write_text(json.dumps(contents), encoding="utf-8")
        return report

    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmpdir.cleanup()


if __name__ == "__main__":
    unittest.main()
