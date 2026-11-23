#!/usr/bin/env python3
"""Tests for lcov_metrics.py."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lcov_metrics
import test_timing

_LCOV_SCRIPT = Path(lcov_metrics.__file__).resolve()

_REFERENCE_LCOV = """\
SF:donner/base/Foo.cc
DA:10,1
DA:11,3
DA:12,1
BRDA:11,0,0,1
BRDA:11,0,1,0
end_of_record
"""


def _report_endpoint_response(files: object) -> dict[str, object]:
    """Return a payload shaped like Codecov's `/report/?sha=<sha>` response."""
    return {
        "totals": {"files": 1, "lines": 3, "hits": 1, "misses": 1, "partials": 1},
        "files": files,
        "commit_file_url": "https://app.codecov.io/github/owner/repo/commit/abc/blob/",
    }


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


class LcovMetricsCommandLineTest(unittest.TestCase):
    """Runs the script as its callers do, asserting exit status and diagnostics.

    Every invalid input must fail before any summary is printed, so no invalid
    reference or empty report can produce a coverage percentage at all.
    """

    def setUp(self) -> None:
        self._tmpdir = tempfile.TemporaryDirectory()
        self._root = Path(self._tmpdir.name)
        self._report = self._root / "filtered_report.dat"
        self._report.write_text(_REFERENCE_LCOV, encoding="utf-8")

    def tearDown(self) -> None:
        self._tmpdir.cleanup()

    def test_valid_report_endpoint_response_limits_metrics_to_reference_lines(self) -> None:
        reference = self._write_reference(
            _report_endpoint_response(
                [{"name": "donner/base/Foo.cc", "line_coverage": [[10, 0], [11, 2], [13, 1]]}]
            )
        )

        result = self._run("--json", "--codecov-reference-json", str(reference))

        self.assertEqual(result.returncode, 0, self._describe(result))
        summary = json.loads(result.stdout)
        self.assertAlmostEqual(summary["codecov_lines"].pop("percent"), 100.0 / 3)
        self.assertEqual(
            summary["codecov_lines"],
            {
                "hits": 1,
                "misses": 1,
                "partials": 1,
                "total": 3,
                "display_percent": 33,
                "target_percent": 90.0,
                "hits_needed_for_target": 2,
            },
        )
        self.assertEqual(summary["branches"]["hit"], 1)
        self.assertEqual(summary["branches"]["found"], 2)

    def test_valid_commit_endpoint_response_is_accepted(self) -> None:
        reference = self._write_reference(
            {
                "commitid": "abc",
                "report": _report_endpoint_response(
                    [{"name": "donner/base/Foo.cc", "line_coverage": [[10, 0], [11, 2]]}]
                ),
            }
        )

        result = self._run("--codecov-reference-json", str(reference))

        self.assertEqual(result.returncode, 0, self._describe(result))
        self.assertIn("codecov lines: 1/2 (50.00%)", result.stdout)

    def test_line_entries_with_extra_fields_are_accepted(self) -> None:
        reference = self._write_reference(
            _report_endpoint_response(
                [{"name": "donner/base/Foo.cc", "line_coverage": [[10, 0, "extra"], [11, 2]]}]
            )
        )

        result = self._run("--codecov-reference-json", str(reference))

        self.assertEqual(result.returncode, 0, self._describe(result))
        self.assertIn("codecov lines: 1/2 (50.00%)", result.stdout)

    def test_metadata_only_json_is_rejected(self) -> None:
        cases = {
            "commit metadata without a report": {
                "commitid": "abc",
                "message": "Commit message",
                "totals": {"coverage": 85.0},
            },
            "commit metadata with a null report": {"commitid": "abc", "report": None},
            "report without a files list": {"report": {"totals": {"coverage": 85.0}}},
            "files is not a list": _report_endpoint_response({"name": "donner/base/Foo.cc"}),
        }
        for name, payload in cases.items():
            with self.subTest(name):
                reference = self._write_reference(payload)

                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), "files", "/report/?sha=")

    def test_codecov_error_response_is_rejected_with_its_detail(self) -> None:
        reference = self._write_reference(
            {"detail": "The commit abc is not in our records. Please specify valid commit."}
        )

        result = self._run("--codecov-reference-json", str(reference))

        self._assert_rejected(result, str(reference), "The commit abc is not in our records")

    def test_untrusted_reference_text_is_escaped_in_diagnostics(self) -> None:
        injected = "x\x1b[31m\n::error::forged"
        cases = {
            "error detail": {"detail": injected},
            "duplicate file name": _report_endpoint_response(
                [
                    {"name": injected, "line_coverage": [[10, 0]]},
                    {"name": injected, "line_coverage": [[11, 0]]},
                ]
            ),
            "file name without line_coverage": _report_endpoint_response([{"name": injected}]),
        }
        for name, payload in cases.items():
            with self.subTest(name):
                reference = self._write_reference(payload)

                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), json.dumps(injected))
                details = self._describe(result)
                self.assertNotIn("\x1b", result.stderr, details)
                self.assertNotIn("\n::error::", result.stderr, details)

    def test_reference_json_beyond_decoder_limits_is_rejected(self) -> None:
        depth = 100_000
        cases = {
            "nesting deeper than the decoder allows": "[" * depth + "]" * depth,
            "integer past the int-to-str digit limit": (
                '{"files": [{"name": "donner/base/Foo.cc", "line_coverage": [['
                + "1" * 5000
                + ", 0]]}]}"
            ),
        }
        for name, contents in cases.items():
            with self.subTest(name):
                reference = self._root / "codecov.json"
                reference.write_text(contents, encoding="utf-8")

                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), "not valid JSON")

    def test_malformed_file_entries_are_rejected(self) -> None:
        cases = {
            "entry is not an object": (["donner/base/Foo.cc"], "files[0]"),
            "missing name": ([{"line_coverage": [[10, 0]]}], "files[0]"),
            "empty name": ([{"name": "", "line_coverage": [[10, 0]]}], "files[0]"),
            "missing line_coverage": ([{"name": "donner/base/Foo.cc"}], "donner/base/Foo.cc"),
            "line_coverage is not a list": (
                [{"name": "donner/base/Foo.cc", "line_coverage": {"10": 0}}],
                "donner/base/Foo.cc",
            ),
            "line entry is not a pair": (
                [{"name": "donner/base/Foo.cc", "line_coverage": [10]}],
                "line_coverage[0]",
            ),
            "line number is a string": (
                [{"name": "donner/base/Foo.cc", "line_coverage": [["10", 0]]}],
                "line_coverage[0]",
            ),
            "line number is a boolean": (
                [{"name": "donner/base/Foo.cc", "line_coverage": [[True, 0]]}],
                "line_coverage[0]",
            ),
            "line number is not positive": (
                [{"name": "donner/base/Foo.cc", "line_coverage": [[0, 0]]}],
                "line_coverage[0]",
            ),
            "duplicate file": (
                [
                    {"name": "donner/base/Foo.cc", "line_coverage": [[10, 0]]},
                    {"name": "donner/base/Foo.cc", "line_coverage": [[11, 0]]},
                ],
                "files[1]",
            ),
        }
        for name, (files, location) in cases.items():
            with self.subTest(name):
                reference = self._write_reference(_report_endpoint_response(files))

                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), location)

    def test_empty_reference_report_is_rejected(self) -> None:
        cases = {
            "no files": ([], "no files"),
            "no coverable lines": (
                [{"name": "donner/base/Foo.cc", "line_coverage": []}],
                "no coverable lines",
            ),
        }
        for name, (files, diagnostic) in cases.items():
            with self.subTest(name):
                reference = self._write_reference(_report_endpoint_response(files))

                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), diagnostic)

    def test_unreadable_reference_is_rejected(self) -> None:
        missing = self._root / "missing-codecov.json"
        not_json = self._root / "not-json.json"
        not_json.write_text("<html>rate limited</html>", encoding="utf-8")
        not_object = self._write_reference([{"name": "donner/base/Foo.cc"}])
        cases = {
            "missing file": (missing, "not found"),
            "invalid JSON": (not_json, "not valid JSON"),
            "top level is not an object": (not_object, "JSON object"),
        }
        for name, (reference, diagnostic) in cases.items():
            with self.subTest(name):
                result = self._run("--codecov-reference-json", str(reference))

                self._assert_rejected(result, str(reference), diagnostic)

    def test_lcov_report_without_line_records_is_rejected(self) -> None:
        cases = {
            "empty file": "",
            "records without DA lines": "SF:donner/base/Foo.cc\nend_of_record\n",
        }
        reference = self._write_reference(
            _report_endpoint_response(
                [{"name": "donner/base/Foo.cc", "line_coverage": [[10, 0], [11, 2]]}]
            )
        )
        option_sets = {
            "text": [],
            "json": ["--json"],
            "text with reference": ["--codecov-reference-json", str(reference)],
            "json with reference": ["--json", "--codecov-reference-json", str(reference)],
        }
        for name, contents in cases.items():
            for options_name, options in option_sets.items():
                with self.subTest(name, options=options_name):
                    self._report.write_text(contents, encoding="utf-8")

                    result = self._run(*options)

                    self._assert_rejected(result, str(self._report), "no executable line")

    def test_missing_lcov_report_names_the_report(self) -> None:
        self._report.unlink()

        result = self._run()

        self._assert_rejected(result, str(self._report), "not found")

    def test_report_without_branch_counters_has_no_branch_percentage(self) -> None:
        self._report.write_text("SF:donner/base/Foo.cc\nDA:1,1\nend_of_record\n", encoding="utf-8")

        result = self._run("--json")

        self.assertEqual(result.returncode, 0, self._describe(result))
        branches = json.loads(result.stdout)["branches"]
        self.assertEqual(
            (branches["hit"], branches["found"], branches["percent"]), (0, 0, None)
        )

    def _write_reference(self, contents: object) -> Path:
        reference = self._root / "codecov.json"
        reference.write_text(json.dumps(contents), encoding="utf-8")
        return reference

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(_LCOV_SCRIPT), str(self._report), *args],
            capture_output=True,
            text=True,
            timeout=test_timing.STALL_GUARD_SECONDS,
            check=False,
        )

    def _assert_rejected(
        self, result: subprocess.CompletedProcess[str], *diagnostic_fragments: str
    ) -> None:
        details = self._describe(result)
        self.assertEqual(result.returncode, 1, details)
        self.assertEqual(result.stdout, "", details)
        self.assertTrue(result.stderr.startswith("ERROR: "), details)
        self.assertNotIn("Traceback", result.stderr, details)
        for fragment in diagnostic_fragments:
            self.assertIn(fragment, result.stderr, details)

    @staticmethod
    def _describe(result: subprocess.CompletedProcess[str]) -> str:
        return (
            f"exit status {result.returncode}\n--- stdout ---\n{result.stdout}"
            f"--- stderr ---\n{result.stderr}"
        )


if __name__ == "__main__":
    unittest.main()
