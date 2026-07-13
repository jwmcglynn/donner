#!/usr/bin/env python3
"""Tests for spec_seed and the shipped SVG2 baseline seed artifacts."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import spec_seed


SPEC_DIR = Path(spec_seed.__file__).resolve().parent / "spec"


def base_requirement() -> dict:
    return {
        "id": "svg2-cr-20181004/painting/paint-order/req-03",
        "spec_revision": "CR-SVG2-20181004",
        "section": "13 Painting",
        "anchor": "PaintOrder",
        "assertion": "paint-order does not change shaping",
        "strength": "must",
        "subject": "user-agent",
        "software_classes": ["interpreter"],
        "processing_modes": ["static"],
        "oracle_kinds": ["png"],
        "test_ids": [],
        "review_state": "reviewed",
        "evidence_state": "missing-test",
        "source_text_hash": "sha256:" + "e" * 64,
        "dependencies": [],
    }


def inventory_with(requirement: dict) -> dict:
    return {
        "schema": "https://donner.graphics/svg2-suite/requirement-v1.schema.json",
        "baseline": "svg2-cr-20181004",
        "requirements": [requirement],
    }


class ShippedSeedTest(unittest.TestCase):
    def test_seed_artifacts_are_valid(self):
        self.assertEqual(spec_seed.check(SPEC_DIR), [])


class RequirementAccountingTest(unittest.TestCase):
    def test_duplicate_requirement_id_rejected(self):
        inventory = inventory_with(base_requirement())
        inventory["requirements"].append(copy.deepcopy(inventory["requirements"][0]))
        errors = spec_seed.validate_requirements(inventory)
        self.assertTrue(any("duplicate requirement id" in error for error in errors), errors)

    def test_covered_pass_without_test_rejected(self):
        requirement = base_requirement()
        requirement["evidence_state"] = "covered-pass"
        errors = spec_seed.validate_requirements(inventory_with(requirement))
        self.assertTrue(any("covered-pass" in error and "links no test" in error for error in errors), errors)

    def test_covered_pass_requires_review(self):
        requirement = base_requirement()
        requirement["evidence_state"] = "covered-pass"
        requirement["test_ids"] = ["donner-svg2/painting/paint-order/tspan-boundary"]
        requirement["review_state"] = "unreviewed"
        errors = spec_seed.validate_requirements(inventory_with(requirement))
        self.assertTrue(any("not reviewed" in error for error in errors), errors)

    def test_draft_dependency_without_dependency_rejected(self):
        requirement = base_requirement()
        requirement["evidence_state"] = "draft-dependency"
        errors = spec_seed.validate_requirements(inventory_with(requirement))
        self.assertTrue(any("draft-dependency" in error and "names no dependency" in error for error in errors), errors)

    def test_unlocked_dependency_rejected(self):
        requirement = base_requirement()
        requirement["dependencies"] = ["css-color-4"]
        errors = spec_seed.validate_requirements(inventory_with(requirement), lock_keys={"unicode"})
        self.assertTrue(any("unlocked dependency" in error for error in errors), errors)

    def test_locked_dependency_accepted(self):
        requirement = base_requirement()
        requirement["dependencies"] = ["css-color-4"]
        errors = spec_seed.validate_requirements(inventory_with(requirement), lock_keys={"css-color-4"})
        self.assertEqual(errors, [])

    def test_baseline_prefix_enforced(self):
        requirement = base_requirement()
        requirement["id"] = "svg2-cr-20181004/painting/paint-order/req-03"
        inventory = inventory_with(requirement)
        inventory["baseline"] = "svg2-cr-99999999"
        errors = spec_seed.validate_requirements(inventory)
        self.assertTrue(any("baseline prefix" in error for error in errors), errors)


class BaselineLockAccountingTest(unittest.TestCase):
    def base_lock(self) -> dict:
        return {
            "dependency_lock": {
                "entries": [
                    {"key": "unicode", "status": "Published", "stable": True},
                    {"key": "css-color-4", "status": "Candidate Recommendation", "stable": False},
                ]
            }
        }

    def test_duplicate_dependency_key_rejected(self):
        lock = self.base_lock()
        lock["dependency_lock"]["entries"].append({"key": "unicode", "status": "Published", "stable": True})
        errors = spec_seed.validate_baseline_lock(lock)
        self.assertTrue(any("duplicate dependency key" in error for error in errors), errors)

    def test_stable_draft_rejected(self):
        lock = self.base_lock()
        lock["dependency_lock"]["entries"][1]["stable"] = True
        errors = spec_seed.validate_baseline_lock(lock)
        self.assertTrue(any("marked stable but its status is a draft" in error for error in errors), errors)

    def test_valid_lock_accepted(self):
        self.assertEqual(spec_seed.validate_baseline_lock(self.base_lock()), [])


if __name__ == "__main__":
    unittest.main()
