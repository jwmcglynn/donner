#!/usr/bin/env python3
"""Tests for manifest_validation."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest_validation


def base_test_entry() -> dict:
    return {
        "id": "donner-svg2/painting/paint-order/tspan-boundary",
        "input": "tests/painting/paint-order/tspan-boundary.svg",
        "oracle": {
            "kind": "png",
            "path": "tests/painting/paint-order/tspan-boundary.png",
            "width": 500,
            "height": 500,
            "provenance": "independently-reviewed-reference",
        },
        "assertion": "paint-order does not split shaping across a paint-only tspan",
        "spec_requirements": ["svg2-cr-20181004/painting/paint-order/req-03"],
        "capabilities": ["text", "paint-order"],
        "resources": ["fonts/NotoSans-Regular.ttf"],
    }


def base_manifest() -> dict:
    return {
        "schema": "https://donner.graphics/svg2-suite/corpus-v1.schema.json",
        "corpus": "donner-svg2",
        "revision": "0000000000000000000000000000000000000000",
        "tests": [base_test_entry()],
    }


class ManifestValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        # Materialize the files the base manifest references.
        for relative in (
            "tests/painting/paint-order/tspan-boundary.svg",
            "tests/painting/paint-order/tspan-boundary.png",
            "fonts/NotoSans-Regular.ttf",
        ):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"content")

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write_manifest(self, manifest: dict) -> Path:
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def validate(self, manifest: dict, **kwargs) -> list[str]:
        return manifest_validation.validate_manifest(self.write_manifest(manifest), **kwargs)

    def test_valid_manifest_passes(self):
        self.assertEqual(self.validate(base_manifest()), [])

    def test_duplicate_id_rejected(self):
        manifest = base_manifest()
        manifest["tests"].append(copy.deepcopy(manifest["tests"][0]))
        errors = self.validate(manifest)
        self.assertTrue(any("duplicate test id" in error for error in errors), errors)

    def test_missing_input_file_rejected(self):
        manifest = base_manifest()
        manifest["tests"][0]["input"] = "tests/painting/paint-order/does-not-exist.svg"
        errors = self.validate(manifest)
        self.assertTrue(any("missing input file" in error for error in errors), errors)

    def test_missing_oracle_file_rejected(self):
        manifest = base_manifest()
        manifest["tests"][0]["oracle"]["path"] = "tests/painting/paint-order/missing.png"
        errors = self.validate(manifest)
        self.assertTrue(any("missing oracle file" in error for error in errors), errors)

    def test_hash_mismatch_rejected(self):
        manifest = base_manifest()
        manifest["integrity"] = {
            "tests/painting/paint-order/tspan-boundary.svg": "sha256:" + "0" * 64
        }
        errors = self.validate(manifest)
        self.assertTrue(any("hash mismatch" in error for error in errors), errors)

    def test_matching_hash_accepted(self):
        import hashlib

        manifest = base_manifest()
        digest = hashlib.sha256(b"content").hexdigest()
        manifest["integrity"] = {
            "tests/painting/paint-order/tspan-boundary.svg": f"sha256:{digest}"
        }
        self.assertEqual(self.validate(manifest), [])

    def test_unknown_requirement_rejected_when_inventory_supplied(self):
        errors = self.validate(base_manifest(), known_requirements={"svg2-cr-20181004/painting/paint-order/req-01"})
        self.assertTrue(any("unknown requirement id" in error for error in errors), errors)

    def test_known_requirement_accepted_when_inventory_supplied(self):
        errors = self.validate(
            base_manifest(),
            known_requirements={"svg2-cr-20181004/painting/paint-order/req-03"},
        )
        self.assertEqual(errors, [])

    def test_missing_required_field_is_structural_error(self):
        manifest = base_manifest()
        del manifest["tests"][0]["assertion"]
        errors = self.validate(manifest)
        self.assertTrue(any("assertion" in error and "required" in error for error in errors), errors)

    def test_unsupported_schema_version_rejected(self):
        manifest = base_manifest()
        manifest["schema"] = "https://donner.graphics/svg2-suite/corpus-v2.schema.json"
        errors = self.validate(manifest)
        self.assertTrue(any("const" in error for error in errors), errors)

    def test_malformed_requirement_id_rejected(self):
        manifest = base_manifest()
        manifest["tests"][0]["spec_requirements"] = ["not-a-real-requirement"]
        errors = self.validate(manifest)
        self.assertTrue(any("pattern" in error for error in errors), errors)

    def test_malformed_json_rejected(self):
        path = self.root / "manifest.json"
        path.write_text("{not json", encoding="utf-8")
        errors = manifest_validation.validate_manifest(path)
        self.assertTrue(any("not valid JSON" in error for error in errors), errors)

    def test_resource_outside_declared_roots_rejected(self):
        manifest = base_manifest()
        manifest["tests"][0]["resources"] = ["tests/painting/paint-order/tspan-boundary.svg"]
        errors = self.validate(manifest)
        self.assertTrue(any("undeclared resource path" in error for error in errors), errors)

    def test_input_outside_declared_roots_rejected(self):
        manifest = base_manifest()
        manifest["tests"][0]["input"] = "resources/sneaky.svg"
        errors = self.validate(manifest)
        self.assertTrue(any("undeclared input path" in error for error in errors), errors)


class ManifestPathSafetyTableTest(unittest.TestCase):
    """Table-driven path-safety negatives applied through the manifest validator."""

    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def validate_with_input(self, input_path: str) -> list[str]:
        manifest = base_manifest()
        manifest["tests"][0]["input"] = input_path
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return manifest_validation.validate_manifest(path)

    def test_path_safety_negatives(self):
        cases = {
            "parent-traversal": "tests/../../etc/passwd",
            "absolute-path": "/etc/passwd",
            "url-http": "http://example.invalid/x.svg",
            "url-file": "file:///etc/passwd",
            "url-data": "data:text/plain,hi",
            "backslash": "tests\\painting\\x.svg",
            "embedded-parent": "tests/painting/../../../secret.svg",
        }
        for name, input_path in cases.items():
            with self.subTest(case=name):
                errors = self.validate_with_input(input_path)
                self.assertTrue(
                    any("unsafe input path" in error for error in errors),
                    f"{name}: expected unsafe path rejection, got {errors}",
                )


if __name__ == "__main__":
    unittest.main()
