#!/usr/bin/env python3
"""Freshness gate for the frozen baseline corpus's structural counters.

The counters come from the CPU path encoder, so this gate needs no GPU device and runs on every
lane. It re-derives them and requires the committed manifest to match byte-for-byte, which is
what makes the frozen set an oracle a replacement backend can be held to rather than a snapshot
nobody can reproduce.
"""

from __future__ import annotations

import argparse
import difflib
import json
import subprocess
import unittest
from pathlib import Path

REGEN_COMMAND = (
    "bazel run //donner/gpu/baseline:dump_baseline_counters > "
    "donner/gpu/baseline/baselines/structural_counters.json"
)


class StructuralCountersTest(unittest.TestCase):
    dump_binary: Path
    committed: Path

    def committed_manifest(self) -> dict:
        return json.loads(self.committed.read_text(encoding="utf-8"))

    def test_committed_counters_match_a_fresh_encode(self) -> None:
        fresh = subprocess.run(
            [str(self.dump_binary)], capture_output=True, text=True, check=True
        ).stdout
        committed = self.committed.read_text(encoding="utf-8")
        if fresh == committed:
            return
        diff = "".join(
            difflib.unified_diff(
                committed.splitlines(keepends=True),
                fresh.splitlines(keepends=True),
                fromfile="committed",
                tofile="freshly encoded",
            )
        )
        self.fail(
            "the frozen structural counters no longer match a fresh encode of the corpus.\n"
            f"{diff}\nIf this change is intended, regenerate with:\n  {REGEN_COMMAND}"
        )

    def test_scene_names_are_unique_and_the_corpus_is_not_empty(self) -> None:
        manifest = self.committed_manifest()
        self.assertEqual(manifest["schemaVersion"], 1)
        names = [scene["name"] for scene in manifest["scenes"]]
        self.assertTrue(names, "the corpus must not be empty")
        self.assertCountEqual(names, set(names), "scene names must be unique")

    def test_the_corpus_freezes_every_admission_outcome(self) -> None:
        manifest = self.committed_manifest()
        outcomes = {path["outcome"] for scene in manifest["scenes"] for path in scene["paths"]}
        self.assertEqual(
            outcomes,
            {"Empty", "Ready", "Rejected"},
            "the corpus must freeze the fail-closed and degenerate outcomes, not only "
            "successfully encoded paths",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump-binary", type=Path, required=True)
    parser.add_argument("--committed", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    StructuralCountersTest.dump_binary = args.dump_binary.resolve()
    StructuralCountersTest.committed = args.committed.resolve()
    unittest.main(argv=[__file__, *unittest_args])
