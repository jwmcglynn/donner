"""Exercise content identity, metadata-only generation, and package corruption failures."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from packaged_resources import generate, merge, resource_record


class PackagedResourcesTest(unittest.TestCase):
    def test_exact_payload_metadata_and_reuse_across_code_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "icon.svg"
            payload = b'<svg xmlns="http://www.w3.org/2000/svg"/>'
            source.write_bytes(payload)
            request = dict(group="chrome", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kIconSvg", source=str(source))])
            generate(request, root / "Icon.h", root / "first")
            generate(request, root / "IconAgain.h", root / "second")
            first = json.loads((root / "first/manifest.json").read_text())
            second = json.loads((root / "second/manifest.json").read_text())
            self.assertEqual(first, second)
            record = first["assets"][0]
            self.assertEqual(record["sha256"], hashlib.sha256(payload).hexdigest())
            self.assertEqual((root / "first" / record["path"]).read_bytes(), payload)
            header = (root / "Icon.h").read_text()
            self.assertNotIn(payload.decode(), header)
            self.assertIn("PackagedResource kIconSvg", header)
            merge([root / "first", root / "second"], root / "bundle")
            self.assertEqual(json.loads((root / "bundle/manifest.json").read_text()), first)
            self.assertEqual(len(list((root / "bundle/assets").iterdir())), 1)

    def test_reject_corruption_and_conflicting_logical_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "font.ttf"
            source.write_bytes(b"encoded-font")
            request = dict(group="ui-font", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kFontTtf", source=str(source))])
            generate(request, root / "Font.h", root / "first")
            source.write_bytes(b"changed-font")
            generate(request, root / "Changed.h", root / "changed")
            with self.assertRaisesRegex(ValueError, "Conflicting"):
                merge([root / "first", root / "changed"], root / "conflict")
            record = json.loads((root / "first/manifest.json").read_text())["assets"][0]
            (root / "first" / record["path"]).write_bytes(b"corrupt-font")
            with self.assertRaisesRegex(ValueError, "identity"):
                merge([root / "first"], root / "corrupt")

    def test_rejects_paths_schema_symlinks_and_extra_inventory_before_reading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "icon.svg"
            source.write_bytes(b"svg-bytes")
            request = dict(group="chrome", runfilesRoot="_main/pkg/set.assets",
                           resources=[dict(id="kIconSvg", source=str(source))])
            generate(request, root / "Icon.h", root / "valid")
            manifest_path = root / "valid/manifest.json"
            original = json.loads(manifest_path.read_text())
            for key, value, reason in [
                    ("path", "../outside.svg", "Noncanonical"),
                    ("encodedSize", True, "size type"),
                    ("encodedSize", 17 * 1024 * 1024, "size"),
                    ("sha256", "bad", "digest"),
                    ("mediaType", "text/javascript", "media type")]:
                with self.subTest(key=key, value=value):
                    altered = json.loads(json.dumps(original))
                    altered["assets"][0][key] = value
                    manifest_path.write_text(json.dumps(altered))
                    with self.assertRaisesRegex(ValueError, reason):
                        merge([root / "valid"], root / ("bad-" + key + str(value)))
            duplicate = json.loads(json.dumps(original))
            duplicate["assets"].append(duplicate["assets"][0])
            manifest_path.write_text(json.dumps(duplicate))
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                merge([root / "valid"], root / "duplicate")
            manifest_path.write_text(json.dumps(original))
            (root / "valid/extra.txt").write_text("unexpected")
            with self.assertRaisesRegex(ValueError, "inventory mismatch"):
                merge([root / "valid"], root / "extra")
            (root / "valid/extra.txt").unlink()
            asset = root / "valid" / original["assets"][0]["path"]
            asset.unlink()
            asset.symlink_to(source)
            with self.assertRaisesRegex(ValueError, "symlink"):
                merge([root / "valid"], root / "symlink")

    def test_bounds_and_disallowed_types(self):
        for identity, extension, payload in [("../escape", "svg", b"x"),
                                             ("kSvg", "js", b"x"), ("kSvg", "svg", b"")]:
            with self.subTest(identity=identity, extension=extension):
                with self.assertRaises(ValueError):
                    resource_record(identity, payload, extension, "chrome")


if __name__ == "__main__":
    unittest.main()
