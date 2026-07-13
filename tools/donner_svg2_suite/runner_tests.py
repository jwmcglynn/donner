#!/usr/bin/env python3
"""Tests for the SVG2 suite reference runner."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from xml.dom import minidom

sys.path.insert(0, str(Path(__file__).resolve().parent))

import jsonschema_lite
import manifest_validation
import runner
import suite_test_support as support


class RunnerScenarioTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.work = self.root / "work"

    def tearDown(self):
        self.temporary_directory.cleanup()

    def _run(self, tests, cases=None, cli_override=None, timeout=30.0):
        manifest_path = support.write_manifest(self.root, tests)
        profile_path = support.write_profile(self.root, cases) if cases is not None else None
        return runner.run_manifest(
            manifest_path,
            support.adapter_argv(),
            profile_path=profile_path,
            work_dir=self.work,
            cli_override=cli_override,
            timeout=timeout,
        )

    def _statuses(self, run):
        return {result["test"]: result["status"] for result in run.results}

    def test_pass_when_output_matches_oracle(self):
        support.make_png(self.root / "tests/a.png")
        support.make_png(self.root / "tests/a.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/a", input_rel="tests/a.png", oracle_rel="tests/a.oracle.png"
        )
        run = self._run([entry])
        self.assertEqual(self._statuses(run), {"donner-svg2/a": "pass"})
        self.assertTrue(run.ok)

    def test_comparison_fail_marks_run_not_ok(self):
        support.make_png(self.root / "tests/m.png")
        support.make_png(self.root / "tests/m.oracle.png", pixel=support.OTHER_PIXEL)
        entry = support.test_entry(
            test_id="donner-svg2/m", input_rel="tests/m.png", oracle_rel="tests/m.oracle.png"
        )
        run = self._run([entry])
        self.assertEqual(self._statuses(run), {"donner-svg2/m": "comparison-fail"})
        self.assertFalse(run.ok)

    def test_unsupported_profile_skips_before_render(self):
        # No input/oracle files exist; unsupported must skip before any file access.
        entry = support.test_entry(
            test_id="donner-svg2/u", input_rel="tests/missing.png", oracle_rel="tests/missing.oracle.png"
        )
        cases = {"donner-svg2/u": {"expectation": "unsupported", "reason_category": "capability", "reason": "no bidi"}}
        run = self._run([entry], cases=cases)
        self.assertEqual(self._statuses(run), {"donner-svg2/u": "unsupported"})
        self.assertTrue(run.ok)

    def test_expected_fail_that_fails_is_healthy(self):
        support.make_png(self.root / "tests/ef.png")
        support.make_png(self.root / "tests/ef.oracle.png", pixel=support.OTHER_PIXEL)
        entry = support.test_entry(
            test_id="donner-svg2/ef", input_rel="tests/ef.png", oracle_rel="tests/ef.oracle.png"
        )
        cases = {"donner-svg2/ef": {"expectation": "expected-fail", "reason_category": "gap", "reason": "known"}}
        run = self._run([entry], cases=cases)
        self.assertEqual(self._statuses(run), {"donner-svg2/ef": "expected-fail"})
        self.assertTrue(run.ok)

    def test_expected_fail_that_passes_fails_the_audit(self):
        support.make_png(self.root / "tests/a.png")
        support.make_png(self.root / "tests/a.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/ef", input_rel="tests/a.png", oracle_rel="tests/a.oracle.png"
        )
        cases = {"donner-svg2/ef": {"expectation": "expected-fail", "reason_category": "gap", "reason": "known"}}
        run = self._run([entry], cases=cases)
        self.assertEqual(self._statuses(run), {"donner-svg2/ef": "comparison-fail"})
        self.assertFalse(run.ok)
        self.assertIn("unexpectedly", run.results[0]["diagnostics"])

    def test_render_only_ignores_oracle_mismatch(self):
        support.make_png(self.root / "tests/ro.png")
        support.make_png(self.root / "tests/ro.oracle.png", pixel=support.OTHER_PIXEL)
        entry = support.test_entry(
            test_id="donner-svg2/ro", input_rel="tests/ro.png", oracle_rel="tests/ro.oracle.png"
        )
        cases = {"donner-svg2/ro": {"expectation": "render-only"}}
        run = self._run([entry], cases=cases)
        self.assertEqual(self._statuses(run), {"donner-svg2/ro": "render-only"})
        self.assertTrue(run.ok)

    def test_pass_fail_skip_are_separated(self):
        support.make_png(self.root / "tests/p.png")
        support.make_png(self.root / "tests/p.oracle.png")
        support.make_png(self.root / "tests/f.png")
        support.make_png(self.root / "tests/f.oracle.png", pixel=support.OTHER_PIXEL)
        tests = [
            support.test_entry(test_id="donner-svg2/p", input_rel="tests/p.png", oracle_rel="tests/p.oracle.png"),
            support.test_entry(test_id="donner-svg2/f", input_rel="tests/f.png", oracle_rel="tests/f.oracle.png"),
            support.test_entry(test_id="donner-svg2/s", input_rel="tests/x.png", oracle_rel="tests/x.oracle.png"),
        ]
        cases = {"donner-svg2/s": {"expectation": "unsupported", "reason_category": "cap", "reason": "n/a"}}
        run = self._run(tests, cases=cases)
        self.assertEqual(
            self._statuses(run),
            {"donner-svg2/p": "pass", "donner-svg2/f": "comparison-fail", "donner-svg2/s": "unsupported"},
        )
        self.assertFalse(run.ok)

    def test_cli_may_not_relax_comparison_budget(self):
        support.make_png(self.root / "tests/a.png")
        support.make_png(self.root / "tests/a.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/a", input_rel="tests/a.png", oracle_rel="tests/a.oracle.png"
        )
        with self.assertRaises(runner.ComparisonRelaxationError):
            self._run([entry], cli_override={"threshold": 0.5, "max_mismatched_pixels": 0})

    def test_resolve_comparison_precedence(self):
        # Corpus default is exact; profile may loosen; CLI may only tighten.
        self.assertEqual(
            runner.resolve_comparison(None, None), {"threshold": 0.0, "max_mismatched_pixels": 0}
        )
        loosened = runner.resolve_comparison(
            {"comparison": {"threshold": 0.02, "max_mismatched_pixels": 100}}, None
        )
        self.assertEqual(loosened, {"threshold": 0.02, "max_mismatched_pixels": 100})
        tightened = runner.resolve_comparison(
            {"comparison": {"threshold": 0.02, "max_mismatched_pixels": 100}},
            {"threshold": 0.0, "max_mismatched_pixels": 0},
        )
        self.assertEqual(tightened, {"threshold": 0.0, "max_mismatched_pixels": 0})
        with self.assertRaises(runner.ComparisonRelaxationError):
            runner.resolve_comparison({"comparison": {"threshold": 0.0}}, {"threshold": 0.1})

    def test_json_output_conforms_to_result_schema(self):
        support.make_png(self.root / "tests/a.png")
        support.make_png(self.root / "tests/a.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/a", input_rel="tests/a.png", oracle_rel="tests/a.oracle.png"
        )
        run = self._run([entry])
        document = runner.result_document(
            run,
            bundle_digest="sha256:" + "a" * 64,
            adapter_id="reference",
            profile_name="none",
            baseline_formal="svg2-cr-20181004",
            dependency_lock="sha256:" + "b" * 64,
            conformance_classes=["interpreter"],
            processing_modes=["static"],
        )
        schema = manifest_validation.load_schema("result-v1.schema.json")
        self.assertEqual(jsonschema_lite.validate(document, schema), [])

    def test_junit_output_is_wellformed(self):
        support.make_png(self.root / "tests/a.png")
        support.make_png(self.root / "tests/a.oracle.png")
        entry = support.test_entry(
            test_id="donner-svg2/a", input_rel="tests/a.png", oracle_rel="tests/a.oracle.png"
        )
        run = self._run([entry])
        parsed = minidom.parseString(runner.junit_document(run))
        self.assertEqual(len(parsed.getElementsByTagName("testcase")), 1)


if __name__ == "__main__":
    unittest.main()
