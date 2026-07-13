#!/usr/bin/env python3
"""Pilot parity test for the Donner SVG2 render adapter (design 0057, Rollout 4).

Drives the portable reference runner over a small resvg subset with the Donner
render adapter and Donner's own image comparison (svg2_image_compare, which
reuses pixelmatch), then asserts every pilot case ends in ``pass``. Because the
adapter renders through the same tiny-skia path as
donner/svg/renderer/tests/resvg_test_suite.cc and the comparison reuses the same
pixelmatch, the runner reproduces the existing fixture's pass/fail outcome for
these cases. The upstream resvg files are referenced in place; nothing is copied
or modified.
"""

import os
import unittest
from pathlib import Path

import runner
from python.runfiles import runfiles


def _runfiles():
    return runfiles.Create()


def _rlocation(handle, path):
    resolved = handle.Rlocation(path)
    if resolved is None or not os.path.exists(resolved):
        raise FileNotFoundError(f"runfiles entry not found: {path!r} (resolved {resolved!r})")
    return resolved


class DonnerSvg2PilotParityTest(unittest.TestCase):
    def setUp(self):
        handle = _runfiles()
        self.adapter = _rlocation(
            handle, "donner/donner/svg/renderer/tests/donner_svg2_render_adapter"
        )
        self.comparator = _rlocation(
            handle, "donner/donner/svg/renderer/tests/svg2_image_compare"
        )
        self.manifest = Path(
            _rlocation(
                handle,
                "donner/donner/svg/renderer/tests/pilot_corpus/manifest.json",
            )
        )
        self.profile = Path(
            _rlocation(
                handle,
                "donner/donner/svg/renderer/tests/pilot_corpus/profile.json",
            )
        )
        # The vendored resvg files land in runfiles as symlinks into the source
        # tree, and the runner's path-safety rejects symlink path components.
        # Resolve the real (non-symlink) corpus root via the upstream LICENSE and
        # reference the tests/oracles/fonts in place.
        license_path = _rlocation(handle, "donner/third_party/resvg-test-suite/LICENSE")
        self.corpus_root = Path(os.path.realpath(license_path)).parent

    def test_pilot_matches_fixture_outcomes(self):
        result = runner.run_manifest(
            self.manifest,
            [self.adapter],
            profile_path=self.profile,
            corpus_root=self.corpus_root,
            comparator_argv=[self.comparator],
            timeout=120.0,
        )

        statuses = {r["test"]: r["status"] for r in result.results}
        self.assertTrue(statuses, "runner produced no results")
        for test_id, status in statuses.items():
            self.assertEqual(
                status,
                "pass",
                f"{test_id} ended in {status!r}, expected pass (parity with the resvg fixture)",
            )
        self.assertTrue(result.ok)


if __name__ == "__main__":
    unittest.main()
