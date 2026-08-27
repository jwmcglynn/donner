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
import re
import subprocess
import unittest
from pathlib import Path

# Provenance keys every pixel capture must record, so a frozen baseline can never be traced back
# to "some machine" and the check mode can tell whether it is looking at the same environment.
REQUIRED_PROVENANCE_KEYS = (
    "schemaVersion",
    "sourceRevision",
    "sourceTreeClean",
    "rendererPath",
    "rendererBackend",
    "adapterName",
    "adapterBackend",
    "adapterType",
    "targetFormat",
    "targetSize",
    "capturedScenes",
)

REGEN_COMMAND = (
    "bazel run //donner/gpu/baseline:dump_baseline_counters > "
    "donner/gpu/baseline/baselines/structural_counters.json"
)


def parse_provenance(text: str) -> dict[str, str]:
    """Parse the `key: value` provenance record, ignoring blank and `#` comment lines."""
    values: dict[str, str] = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition(":")
        if not separator:
            raise ValueError(f"provenance line {number} is not `key: value`: {raw!r}")
        values[key.strip()] = value.strip()
    return values


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


    def frozen_environments(self) -> list[Path]:
        return sorted(self.committed.parent.glob("*/capture_provenance.txt"))

    def test_at_least_one_environment_is_frozen(self) -> None:
        self.assertTrue(
            self.frozen_environments(),
            "no capture provenance found; the pixel freeze needs at least one adapter",
        )

    def test_every_environment_records_its_capture(self) -> None:
        for record in self.frozen_environments():
            values = parse_provenance(record.read_text(encoding="utf-8"))
            for key in REQUIRED_PROVENANCE_KEYS:
                self.assertIn(key, values, f"{record.parent.name} provenance is missing {key!r}")
                self.assertNotEqual(
                    values[key], "", f"{record.parent.name} provenance {key!r} is empty"
                )

    def test_every_environment_names_the_revision_it_was_captured_at(self) -> None:
        # A baseline nobody can trace to a revision cannot be re-derived, so it is not an oracle.
        # A run that bootstraps a new environment from inside the test suite cannot know the
        # revision, and leaves a placeholder; this is what stops that placeholder from landing.
        for record in self.frozen_environments():
            values = parse_provenance(record.read_text(encoding="utf-8"))
            self.assertRegex(
                values["sourceRevision"],
                r"^[0-9a-f]{40}$",
                f"{record.parent.name} provenance does not name the revision it was captured at",
            )
            self.assertIn(values["sourceTreeClean"], ("clean", "dirty"), record.parent.name)

    def test_every_environment_captures_only_scenes_the_corpus_defines(self) -> None:
        corpus_names = {scene["name"] for scene in self.committed_manifest()["scenes"]}
        for record in self.frozen_environments():
            values = parse_provenance(record.read_text(encoding="utf-8"))
            captured = [name for name in values["capturedScenes"].split(",") if name]
            self.assertTrue(captured, f"{record.parent.name} provenance lists no scenes")
            self.assertTrue(
                set(captured).issubset(corpus_names),
                f"{record.parent.name} provenance names scenes the corpus does not define: "
                f"{sorted(set(captured) - corpus_names)}",
            )

    def test_every_environment_has_a_png_for_every_scene_it_claims(self) -> None:
        for record in self.frozen_environments():
            values = parse_provenance(record.read_text(encoding="utf-8"))
            for name in (n for n in values["capturedScenes"].split(",") if n):
                png = record.parent / f"{name}.png"
                self.assertTrue(png.is_file(), f"{record.parent.name} is missing {png.name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dump-binary", type=Path, required=True)
    parser.add_argument("--committed", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    StructuralCountersTest.dump_binary = args.dump_binary.resolve()
    StructuralCountersTest.committed = args.committed.resolve()
    unittest.main(argv=[__file__, *unittest_args])
