#!/usr/bin/env python3
"""Tests for resvg_base_manifest.

Generates the resvg base manifest from the real vendored tree
(``third_party/resvg-test-suite``) and validates it with the slice-1
validator, over the actual files on disk (not fixtures), and proves the
generator is deterministic.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from python.runfiles import runfiles

import manifest_validation
import resvg_base_manifest


def _find_resvg_root() -> Path:
    """Locate the physical, non-symlinked vendored resvg-test-suite directory.

    Bazel's runfiles tree for a vendored (non-generated) source file is a
    symlink pointing at the real file in the workspace; path_safety rejects
    symlink path components (by design: a symlink is an escape vector), so
    validating a generated manifest directly against the runfiles-tree path
    would fail every single referenced file. Resolving through the runfiles
    indirection once here, via the LICENSE file that
    third_party/resvg-test-suite/BUILD.bazel already exports, yields the real
    on-disk directory: an ordinary directory of ordinary files with no
    symlinks in it, which is what the manifest actually describes.
    """

    r = runfiles.Create()
    anchor = r.Rlocation("donner/third_party/resvg-test-suite/LICENSE")
    if not anchor:
        raise RuntimeError(
            "could not locate third_party/resvg-test-suite/LICENSE in runfiles; "
            "is //third_party/resvg-test-suite:LICENSE in this test's data?"
        )
    return Path(os.path.realpath(anchor)).parent


class ResvgBaseManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.resvg_root = _find_resvg_root()
        cls.manifest = resvg_base_manifest.build_manifest(cls.resvg_root)

    def test_generated_manifest_validates_against_vendored_tree(self):
        manifest_text = resvg_base_manifest.render_manifest(self.manifest)
        with tempfile.TemporaryDirectory() as tmp:
            # The manifest is written somewhere that is *not* inside the
            # vendored tree, to exercise the corpus_root override rather than
            # the manifest-path-parent default.
            manifest_path = Path(tmp) / "generated-manifest.json"
            manifest_path.write_text(manifest_text, encoding="utf-8")
            errors = manifest_validation.validate_manifest(
                manifest_path, corpus_root=self.resvg_root
            )
        self.assertEqual(errors, [], errors)

    def test_generation_is_deterministic(self):
        first = resvg_base_manifest.render_manifest(
            resvg_base_manifest.build_manifest(self.resvg_root)
        )
        second = resvg_base_manifest.render_manifest(
            resvg_base_manifest.build_manifest(self.resvg_root)
        )
        self.assertEqual(first, second)

    def test_every_id_is_namespaced_under_resvg(self):
        for test in self.manifest["tests"]:
            self.assertTrue(test["id"].startswith("resvg/"), test["id"])

    def test_test_count_exceeds_expected_floor(self):
        self.assertGreater(len(self.manifest["tests"]), 1000)

    def test_known_case_is_present(self):
        ids = {test["id"] for test in self.manifest["tests"]}
        self.assertIn("resvg/shapes/rect/simple-case", ids)

    def test_integrity_map_is_nonempty_and_covers_inputs_and_oracles(self):
        integrity = self.manifest["integrity"]
        self.assertGreater(len(integrity), 0)
        for test in self.manifest["tests"]:
            self.assertIn(test["input"], integrity)
            self.assertIn(test["oracle"]["path"], integrity)

    def test_bundle_metadata(self):
        manifest_text = resvg_base_manifest.render_manifest(self.manifest)
        bundle = resvg_base_manifest.build_bundle(self.manifest, manifest_text)
        self.assertEqual(bundle["source"]["revision"], resvg_base_manifest.DEFAULT_REVISION)
        self.assertEqual(bundle["source"]["revision"], self.manifest["revision"])
        self.assertEqual(bundle["license"]["id"], "MIT")
        self.assertEqual(bundle["license"]["file"], "LICENSE")
        self.assertTrue(bundle["manifest_sha256"].startswith("sha256:"))
        self.assertEqual(bundle["test_count"], len(self.manifest["tests"]))

    def test_bundle_metadata_round_trips_through_json(self):
        manifest_text = resvg_base_manifest.render_manifest(self.manifest)
        bundle = resvg_base_manifest.build_bundle(self.manifest, manifest_text)
        rendered = resvg_base_manifest.render_bundle(bundle)
        reparsed = json.loads(rendered)
        self.assertEqual(reparsed, bundle)


if __name__ == "__main__":
    unittest.main()
