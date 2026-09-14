#!/usr/bin/env python3
"""Verify one complete pinned font, keeping catalog cases independently bounded."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from catalog_generate import encode_font, read_bounded, sha256, verify_preserved_tables


class CatalogWoff2AssetTest(unittest.TestCase):
    def test_reencoding_preserves_the_complete_font(self):
        manifest = json.loads(Path(ARGS.manifest).read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["fonts"]), 12)
        matches = [font for font in manifest["fonts"] if font["source"]["repo"] == ARGS.repo]
        self.assertEqual(len(matches), 1)
        metadata = matches[0]
        self.assertEqual(metadata["path"], "fonts/" + metadata["sha256"] + ".woff2")
        original = read_bounded(ARGS.source)
        self.assertEqual(sha256(original), metadata["source"]["sha256"])
        self.assertEqual(len(original), metadata["source"]["bytes"])
        packaged = read_bounded(Path(ARGS.assets) / metadata["path"])
        self.assertEqual(sha256(packaged), metadata["sha256"])
        self.assertEqual(len(packaged), metadata["encoded_bytes"])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for attempt in range(2):
                with self.subTest(attempt=attempt, family=metadata["family"]):
                    encoded_path = root / f"encoded-{attempt}.woff2"
                    decoded_path = root / f"decoded-{attempt}.ttf"
                    encode_font(ARGS.encoder, ARGS.source, encoded_path, decoded_path)
                    self.assertEqual(read_bounded(encoded_path), packaged)
                    decoded = read_bounded(decoded_path)
                    self.assertEqual(len(decoded), metadata["decoded_bytes"])
                    self.assertEqual(sha256(decoded), metadata["decoded_sha256"])
                    verify_preserved_tables(original, decoded)
                    subprocess.run(
                        [str(Path(ARGS.compare).resolve()), ARGS.source, str(decoded_path)],
                        check=True,
                        timeout=30,
                    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "assets", "repo", "source", "encoder", "compare"):
        parser.add_argument("--" + name, required=True)
    ARGS, unittest_args = parser.parse_known_args()
    unittest.main(argv=[__file__] + unittest_args)
