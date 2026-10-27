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

    def test_bounds_and_disallowed_types(self):
        for identity, extension, payload in [("../escape", "svg", b"x"),
                                             ("kSvg", "js", b"x"), ("kSvg", "svg", b"")]:
            with self.subTest(identity=identity, extension=extension):
                with self.assertRaises(ValueError):
                    resource_record(identity, payload, extension, "chrome")


if __name__ == "__main__":
    unittest.main()
