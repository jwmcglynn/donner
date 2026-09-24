"""Tests for the path-safe per-target proof retained with coverage reports."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import coverage_run_proof as proof


_REVISION = "a" * 40
_LABEL = "//donner/editor/repro:replay_resource_budget_tests"
_SECRET_URI = "file:///private/runner-host/secret-path/test.log"


class CoverageRunProofTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bep = self.root / "bep.json"
        self.report = self.root / "filtered_report.dat"
        self.report.write_text(
            "SF:donner/editor/repro/ReplayResourceBudget.h\n"
            "DA:1,1\n"
            "DA:2,0\n"
            "BRDA:1,0,0,1\n"
            "BRDA:1,0,1,0\n"
            "end_of_record\n",
            encoding="utf-8",
        )
        self.write_bep()

    def write_bep(self, *, complete=True, status="PASSED"):
        events = [
            {
                "id": {"testSummary": {"label": _LABEL, "configuration": {"id": "abc123"}}},
                "testSummary": {"overallStatus": status, "passed": [{"uri": _SECRET_URI}]},
            },
            {
                "id": {
                    "targetConfigured": {
                        "label": "//donner/editor:platform_only_test",
                        "configuration": {"id": "abc123"},
                    }
                },
                "aborted": {"reason": "SKIPPED"},
            },
            {
                "id": {"buildFinished": {}},
                "finished": {"overallSuccess": True, "exitCode": {"name": "SUCCESS"}},
            },
        ]
        if complete:
            events.append({"id": {"buildMetrics": {}}, "lastMessage": True})
        self.bep.write_text(
            "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8"
        )

    def make_proof(self, **changes):
        arguments = {
            "bep": self.bep,
            "report": self.report,
            "reason": "non_pr",
            "patterns": "//donner/...",
            "expected_pattern_count": 1,
            "event": "push",
            "ref": "refs/heads/main",
            "revision": _REVISION,
        }
        arguments.update(changes)
        return proof.make_proof(**arguments)

    def test_complete_main_proof_retains_status_and_line_universe_without_uris(self):
        result = self.make_proof()
        self.assertEqual(result["scope"], "complete-main")
        self.assertEqual(result["selection"], {
            "reason": "non_pr", "patterns": ["//donner/..."], "pattern_count": 1
        })
        self.assertEqual(result["test_statuses"], [
            {"label": _LABEL, "configuration": "abc123", "status": "PASSED"}
        ])
        self.assertEqual(result["skipped_targets"], [
            {
                "label": "//donner/editor:platform_only_test",
                "configuration": "abc123",
                "status": "SKIPPED",
            }
        ])
        self.assertEqual(result["report"]["source_files"], 1)
        self.assertEqual(result["report"]["executable_lines"], 2)
        self.assertEqual(result["report"]["fully_covered_lines"], 0)
        self.assertEqual(result["report"]["partial_lines"], 1)
        self.assertEqual(result["report"]["missed_lines"], 1)
        self.assertEqual(result["line_universe"], [
            {"source_file": "donner/editor/repro/ReplayResourceBudget.h", "lines": [1, 2]}
        ])
        self.assertNotIn(_SECRET_URI, json.dumps(result))
        self.assertIn("Complete main baseline", proof.summary_markdown(result))

    def test_pull_request_report_is_explicitly_partial(self):
        result = self.make_proof(
            reason="bazel_diff",
            patterns="//donner/base:base_tests",
            event="pull_request",
            ref="refs/pull/123/merge",
        )
        self.assertEqual(result["scope"], "partial-pr")
        self.assertIn(
            "not a project coverage percentage", proof.summary_markdown(result)
        )

    def test_rejects_incomplete_or_failed_bep(self):
        self.write_bep(complete=False)
        with self.assertRaisesRegex(ValueError, "complete successful"):
            self.make_proof()
        self.write_bep(status="FAILED")
        with self.assertRaisesRegex(ValueError, "failed or incomplete"):
            self.make_proof()

    def test_rejects_partial_main_selection_and_unmatched_pattern_count(self):
        with self.assertRaisesRegex(ValueError, "complete product target tree"):
            self.make_proof(reason="coverage_smoke", patterns="//donner/base/...")
        with self.assertRaisesRegex(ValueError, "count does not match"):
            self.make_proof(expected_pattern_count=2)

    def test_rejects_private_lcov_paths_and_unsafe_labels(self):
        self.report.write_text(
            "SF:/private/runner-host/src.cc\nDA:1,1\nend_of_record\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "non-public source path"):
            self.make_proof()

        self.report.write_text(
            "SF:donner/editor/repro/ReplayResourceBudget.h\nDA:1,1\nend_of_record\n",
            encoding="utf-8",
        )
        events = [json.loads(line) for line in self.bep.read_text().splitlines()]
        events[0]["id"]["testSummary"]["label"] = "//donner/evil\nsecret:target"
        self.bep.write_text(
            "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "invalid test summary"):
            self.make_proof()

    def test_cli_writes_a_report_and_summary_without_raw_bep_paths(self):
        artifact = self.root / "coverage-proof.json"
        summary = self.root / "summary.md"
        status = proof.main([
            "--bep", str(self.bep),
            "--report", str(self.report),
            "--reason", "non_pr",
            "--patterns", "//donner/...",
            "--expected-pattern-count", "1",
            "--event", "push",
            "--ref", "refs/heads/main",
            "--revision", _REVISION,
            "--output", str(artifact),
            "--step-summary", str(summary),
        ])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(artifact.read_text())["test_counts"], {"PASSED": 1})
        self.assertNotIn(_SECRET_URI, artifact.read_text())
        self.assertIn("Processed LCOV universe: 1 source files", summary.read_text())


if __name__ == "__main__":
    unittest.main()
