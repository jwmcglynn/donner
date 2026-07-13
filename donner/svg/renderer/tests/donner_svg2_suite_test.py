#!/usr/bin/env python3
"""Donner adapter against the SVG2 extension seed corpus (design 0057, M4).

Drives the portable reference runner over the Donner-authored SVG2 extension
corpus with the Donner render adapter, and asserts every case renders to an exact
match against its manually-constructed geometric reference (the canonical oracle,
compared at the corpus exact budget). These cases isolate SVG 2 behavior the
resvg base suite does not: the initial fill color, fill-opacity clamping, a
zero-width stroke, a negative-dimension parse error, a never-filled line, and the
default paint-order. The upstream resvg gate and the pilot parity test are
unaffected.
"""

import os
import unittest
from pathlib import Path

import runner
from python.runfiles import runfiles


def _rlocation(handle, path):
    resolved = handle.Rlocation(path)
    if resolved is None or not os.path.exists(resolved):
        raise FileNotFoundError(f"runfiles entry not found: {path!r} (resolved {resolved!r})")
    return resolved


class DonnerSvg2SuiteTest(unittest.TestCase):
    def setUp(self):
        handle = runfiles.Create()
        self.adapter = _rlocation(
            handle, "donner/donner/svg/renderer/tests/donner_svg2_render_adapter"
        )
        manifest = _rlocation(
            handle, "donner/donner/svg/renderer/tests/donner_svg2_corpus/manifest.json"
        )
        # Committed corpus files land in runfiles as symlinks into the source
        # tree, and the runner's path-safety rejects symlink path components.
        # Resolve the real (non-symlink) corpus root and reference inputs and
        # oracles in place.
        self.corpus_root = Path(os.path.realpath(manifest)).parent
        self.manifest = self.corpus_root / "manifest.json"

    def test_every_extension_case_matches_its_reference(self):
        result = runner.run_manifest(
            self.manifest,
            [self.adapter],
            corpus_root=self.corpus_root,
            timeout=120.0,
        )
        statuses = {r["test"]: r["status"] for r in result.results}
        self.assertTrue(statuses, "runner produced no results")
        self.assertEqual(len(statuses), 6, statuses)
        for test_id, status in statuses.items():
            self.assertEqual(
                status,
                "pass",
                f"{test_id} ended in {status!r}, expected an exact match against its reference",
            )
        self.assertTrue(result.ok)


if __name__ == "__main__":
    unittest.main()
