#!/usr/bin/env python3
"""Check that compiled metadata, native declarations and packaged files agree."""

import argparse
import hashlib
import json
from pathlib import Path
import unittest


class CatalogMetadataTest(unittest.TestCase):
    def test_metadata_native_payloads_and_web_package_match(self):
        manifest = json.loads(Path(ARGS.manifest).read_text(encoding="utf-8"))
        pins = json.loads(Path(ARGS.pins).read_text(encoding="utf-8"))
        assets = Path(ARGS.assets)
        self.assertEqual(len(manifest["fonts"]), 12)
        self.assertEqual(manifest["source_commit"], pins["commit"])
        self.assertEqual(
            json.loads((assets / "catalog-fonts.json").read_text(encoding="utf-8")), manifest,
        )
        metadata = Path(ARGS.metadata).read_text(encoding="utf-8")
        header = Path(ARGS.header).read_text(encoding="utf-8")
        notices = Path(ARGS.notices).read_text(encoding="utf-8")
        self.assertEqual((assets / "CatalogFontNotices.txt").read_text(encoding="utf-8"), notices)
        expected_paths = {"catalog-fonts.json", "CatalogFontNotices.txt"}
        entries = []
        total_encoded = 0
        for record, pin in zip(manifest["fonts"], pins["fonts"], strict=True):
            with self.subTest(family=pin["family"]):
                self.assertEqual(record["family"], pin["family"])
                self.assertEqual(record["category"], pin["category"])
                self.assertEqual(record["native_symbol"], pin["var"])
                self.assertEqual(record["source"], {
                    key: pin[key] for key in ("repo", "file", "url", "sha256", "bytes")
                })
                self.assertEqual(record["format"], "woff2")
                self.assertEqual(record["path"], "fonts/" + record["sha256"] + ".woff2")
                self.assertLessEqual(record["encoded_bytes"], 2 * 1024 * 1024)
                self.assertLessEqual(record["decoded_bytes"], 2 * 1024 * 1024)
                self.assertEqual(record["decoded_bytes"], record["decoded_size_limit"])
                data = (assets / record["path"]).read_bytes()
                self.assertEqual(len(data), record["encoded_bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), record["sha256"])
                entries.append("DONNER_GF_ENTRY(" + ", ".join([
                    json.dumps(pin["family"]), pin["category"], json.dumps(record["sha256"]),
                    json.dumps(record["path"]), str(len(data)), str(record["decoded_bytes"]),
                    pin["var"],
                ]) + ")")
                self.assertIn(
                    f'extern const unsigned char {pin["var"]}Data[{len(data)}];', header,
                )
                self.assertIn(pin["family"], notices)
                self.assertIn(pin["url"], notices)
                expected_paths.add(record["path"])
                total_encoded += len(data)
        self.assertEqual(
            [line for line in metadata.splitlines() if line.startswith("DONNER_GF_ENTRY(")],
            entries,
        )
        self.assertEqual(manifest["encoded_bytes"], total_encoded)
        self.assertLessEqual(total_encoded, 4 * 1024 * 1024)
        self.assertLessEqual(manifest["memory_budget"]["calculated_transient_bytes"], 32 * 1024 * 1024)
        self.assertEqual(manifest["encoder"]["roundtrip_brotli_memory_limit"], 8 * 1024 * 1024)
        self.assertEqual(
            {path.relative_to(assets).as_posix() for path in assets.rglob("*") if path.is_file()},
            expected_paths,
        )
        self.assertEqual(notices.count("SIL OPEN FONT LICENSE"), 12)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("manifest", "pins", "assets", "metadata", "header", "notices"):
        parser.add_argument("--" + name, required=True)
    ARGS, unittest_args = parser.parse_known_args()
    unittest.main(argv=[__file__] + unittest_args)
