"""Fail when a shipped WGSL artifact is omitted from the validation catalog."""

from pathlib import Path
import re
import unittest


PACKAGE = Path(__file__).resolve().parent.parent


class WgslProjectionCensusTest(unittest.TestCase):
    def test_catalog_matches_production_artifact_targets(self):
        build = (PACKAGE / "BUILD.bazel").read_text()
        catalog = (PACKAGE / "tests/ShippedWgslCatalog.cc").read_text()
        declared = re.findall(r'name = "([a-z0-9_]+_artifact)"', build)
        shipped = {
            name
            for name in declared
            if not name.endswith(("_native_artifact", "_test_artifact"))
        }
        listed = re.findall(r'ShippedWgslCase\{"([a-z0-9_]+_artifact)"', catalog)
        self.assertEqual(len(declared), len(set(declared)), "duplicate artifact target")
        self.assertEqual(len(listed), len(set(listed)), "duplicate catalog family")
        self.assertEqual(shipped, set(listed))
        self.assertTrue(shipped)


if __name__ == "__main__":
    unittest.main()
