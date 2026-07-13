#!/usr/bin/env python3
"""Tests for spec_coverage_lint and the shipped SVG2 coverage graph."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import spec_coverage_lint


SPEC_DIR = Path(spec_coverage_lint.__file__).resolve().parent / "spec"
# The reviewed resvg->requirement mapping lives in the Donner pilot corpus. Both
# the bazel runfiles tree and a plain source checkout place it two levels above
# the tools/donner_svg2_suite package.
WORKSPACE_ROOT = Path(spec_coverage_lint.__file__).resolve().parents[2]
PILOT_MANIFEST = WORKSPACE_ROOT / "donner/svg/renderer/tests/pilot_corpus/manifest.json"
EXTENSION_MANIFEST = WORKSPACE_ROOT / "donner/svg/renderer/tests/donner_svg2_corpus/manifest.json"


def requirement(
    req_id: str = "svg2-cr-20181004/painting/paint-order/req-01",
    *,
    evidence_state: str = "missing-test",
    review_state: str = "reviewed",
    test_ids: list[str] | None = None,
    processing_modes: list[str] | None = None,
    rationale: str = "seed record",
    anchor: str = "PaintOrder",
    section: str = "13 Painting",
) -> dict:
    return {
        "id": req_id,
        "spec_revision": "CR-SVG2-20181004",
        "section": section,
        "anchor": anchor,
        "assertion": "assertion",
        "strength": "must",
        "subject": "user-agent",
        "software_classes": ["interpreter"],
        "processing_modes": processing_modes or ["static", "secure-static"],
        "oracle_kinds": ["png"],
        "test_ids": test_ids or [],
        "review_state": review_state,
        "evidence_state": evidence_state,
        "rationale": rationale,
        "source_text_hash": "sha256:" + "a" * 64,
    }


def inventory(requirements: list[dict]) -> dict:
    return {
        "schema": "https://donner.graphics/svg2-suite/requirement-v1.schema.json",
        "baseline": "svg2-cr-20181004",
        "requirements": requirements,
    }


def corpus(name: str, tests: list[dict]) -> dict:
    return {"schema": "corpus", "corpus": name, "revision": "r", "tests": tests}


def test_case(test_id: str, requirements: list[str]) -> dict:
    return {
        "id": test_id,
        "input": "tests/x.svg",
        "oracle": {"kind": "png", "path": "tests/x.png", "width": 1, "height": 1, "provenance": "p"},
        "assertion": "a",
        "spec_requirements": requirements,
        "capabilities": [],
    }


class ShippedGraphTest(unittest.TestCase):
    def test_committed_graph_lints_clean(self):
        # The committed inventories, cross-checked against the reviewed resvg
        # pilot corpus and the Donner-authored extension corpus, form a consistent
        # bidirectional graph: every covered-state mapping is mutual and its test
        # exists.
        self.assertTrue(PILOT_MANIFEST.is_file(), f"pilot manifest not staged at {PILOT_MANIFEST}")
        self.assertTrue(EXTENSION_MANIFEST.is_file(), f"extension manifest not staged at {EXTENSION_MANIFEST}")
        errors = spec_coverage_lint.check(SPEC_DIR, [PILOT_MANIFEST, EXTENSION_MANIFEST])
        self.assertEqual(errors, [], errors)

    def test_inventory_only_check_is_clean(self):
        # Inventory-only checking (no corpus universe) still passes: it validates
        # states, anchors, and covered-pass integrity from the inventories alone.
        errors = spec_coverage_lint.check(SPEC_DIR)
        self.assertEqual(errors, [], errors)

    def test_artifacts_account_only_covered_pass_as_passing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self.assertEqual(spec_coverage_lint.emit_artifacts(SPEC_DIR, out), [])
            gaps = json.loads((out / "gaps.json").read_text())
            summary = gaps["summary"]
            # Passing evidence is exactly the covered-pass mappings; every other
            # testable state remains a visible gap.
            self.assertEqual(summary["covered_pass"] + summary["gaps"] + summary["not_applicable"], summary["total"])
            self.assertEqual(summary["covered_pass"], 14)
            self.assertGreaterEqual(summary["total"], 35)
            self.assertTrue((out / "requirements.json").is_file())
            self.assertTrue((out / "coverage.json").is_file())


class PositiveGraphTest(unittest.TestCase):
    def test_mutual_covered_pass_edge_is_clean(self):
        req = requirement(evidence_state="covered-pass", test_ids=["donner-svg2/painting/paint-order/case"])
        test = test_case("donner-svg2/painting/paint-order/case", [req["id"]])
        errors = spec_coverage_lint.lint([inventory([req])], [corpus("donner-svg2", [test])])
        self.assertEqual(errors, [], errors)

    def test_missing_test_may_record_candidate_id(self):
        # A gap requirement may name a not-yet-authored candidate test id.
        req = requirement(test_ids=["donner-svg2/painting/paint-order/future"])
        errors = spec_coverage_lint.lint([inventory([req])], [])
        self.assertEqual(errors, [], errors)

    def test_base_corpus_unmapped_case_is_not_orphan(self):
        test = test_case("resvg/shapes/rect/simple-case", [])
        errors = spec_coverage_lint.lint([inventory([requirement()])], [corpus("resvg", [test])])
        self.assertEqual(errors, [], errors)


class NegativeGraphTest(unittest.TestCase):
    def _assert_flags(self, errors: list[str], needle: str) -> None:
        self.assertTrue(any(needle in error for error in errors), f"expected {needle!r} in {errors}")

    def test_duplicate_requirement_id_rejected(self):
        req = requirement()
        errors = spec_coverage_lint.lint([inventory([req, copy.deepcopy(req)])], [])
        self._assert_flags(errors, "duplicate requirement id")

    def test_orphan_extension_test_rejected(self):
        # A donner-svg2 (non-base) case that maps no requirement is an orphan.
        test = test_case("donner-svg2/painting/paint-order/orphan", [])
        errors = spec_coverage_lint.lint([inventory([requirement()])], [corpus("donner-svg2", [test])])
        self._assert_flags(errors, "orphaned test")

    def test_unknown_requirement_id_rejected(self):
        test = test_case(
            "donner-svg2/painting/paint-order/case",
            ["svg2-cr-20181004/painting/paint-order/req-99"],
        )
        errors = spec_coverage_lint.lint([inventory([requirement()])], [corpus("donner-svg2", [test])])
        self._assert_flags(errors, "unknown requirement id")

    def test_skip_counted_as_pass_rejected(self):
        # Marking a requirement covered-pass without a real linked test is the
        # "skip counted as pass" antipattern; the lint must reject it.
        req = requirement(evidence_state="covered-pass", test_ids=[])
        errors = spec_coverage_lint.lint([inventory([req])], [])
        self._assert_flags(errors, "covered-pass but links no test")

    def test_covered_pass_unreviewed_rejected(self):
        req = requirement(
            evidence_state="covered-pass",
            review_state="unreviewed",
            test_ids=["donner-svg2/painting/paint-order/case"],
        )
        test = test_case("donner-svg2/painting/paint-order/case", [req["id"]])
        errors = spec_coverage_lint.lint([inventory([req])], [corpus("donner-svg2", [test])])
        self._assert_flags(errors, "not reviewed")

    def test_covered_pass_dangling_test_rejected(self):
        # With a corpus universe present, a covered-pass whose linked test is
        # absent from it is a dangling claim.
        req = requirement(evidence_state="covered-pass", test_ids=["donner-svg2/painting/paint-order/ghost"])
        other = test_case("resvg/shapes/rect/simple-case", [])
        errors = spec_coverage_lint.lint([inventory([req])], [corpus("resvg", [other])])
        self._assert_flags(errors, "exists in no corpus")

    def test_non_mutual_edge_from_test_rejected(self):
        # Test maps the requirement, requirement does not list the test back.
        req = requirement(test_ids=[])
        test = test_case("donner-svg2/painting/paint-order/case", [req["id"]])
        errors = spec_coverage_lint.lint([inventory([req])], [corpus("donner-svg2", [test])])
        self._assert_flags(errors, "non-mutual coverage edge")

    def test_missing_test_with_existing_test_rejected(self):
        # A missing-test requirement whose candidate is actually authored is stale.
        req = requirement(evidence_state="missing-test", test_ids=["donner-svg2/painting/paint-order/case"])
        test = test_case("donner-svg2/painting/paint-order/case", [req["id"]])
        errors = spec_coverage_lint.lint([inventory([req])], [corpus("donner-svg2", [test])])
        self._assert_flags(errors, "marked missing-test but its linked test")

    def test_stale_anchor_group_rejected(self):
        a = requirement("svg2-cr-20181004/painting/paint-order/req-01", anchor="PaintOrder", section="13 Painting")
        b = requirement("svg2-cr-20181004/painting/paint-order/req-02", anchor="DifferentAnchor", section="13 Painting")
        errors = spec_coverage_lint.lint([inventory([a, b])], [])
        self._assert_flags(errors, "stale anchor")

    def test_disjoint_processing_modes_rejected(self):
        static_req = requirement(
            "svg2-cr-20181004/painting/paint-order/req-01",
            evidence_state="covered-pass",
            processing_modes=["static"],
            test_ids=["donner-svg2/painting/paint-order/case"],
        )
        dynamic_req = requirement(
            "svg2-cr-20181004/interact/events/req-01",
            evidence_state="covered-pass",
            processing_modes=["dynamic-interactive"],
            test_ids=["donner-svg2/painting/paint-order/case"],
            anchor="Events",
            section="15 Scripting",
        )
        test = test_case(
            "donner-svg2/painting/paint-order/case",
            [static_req["id"], dynamic_req["id"]],
        )
        errors = spec_coverage_lint.lint(
            [inventory([static_req, dynamic_req])], [corpus("donner-svg2", [test])]
        )
        self._assert_flags(errors, "disjoint processing modes")

    def test_not_applicable_without_rationale_rejected(self):
        req = requirement(evidence_state="not-applicable", rationale="  ")
        errors = spec_coverage_lint.lint([inventory([req])], [])
        self._assert_flags(errors, "records no rationale")

    def test_duplicate_test_id_across_corpora_rejected(self):
        test = test_case("resvg/shapes/rect/simple-case", [])
        errors = spec_coverage_lint.lint(
            [inventory([requirement()])],
            [corpus("resvg", [test]), corpus("resvg-extra", [copy.deepcopy(test)])],
        )
        self._assert_flags(errors, "duplicate test id across corpora")


if __name__ == "__main__":
    unittest.main()
