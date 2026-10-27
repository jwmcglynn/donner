"""Run the actual example after relocation without source-tree resource lookup."""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class ResourceDeliveryFixture(unittest.TestCase):
    def setUp(self):
        resolver = runfiles.Create()
        self.binary = Path(resolver.Rlocation("donner/examples/svg_text_interaction"))
        assets = resolver.Rlocation("donner/examples/svg_text_interaction.assets")
        self.assets = Path(assets) if assets else None
        self.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR"))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / "relocated"
        self.install.mkdir()
        self.cwd = self.root / "unrelated-cwd"
        self.cwd.mkdir()
        self.executable = self.install / "svg_text_interaction"
        shutil.copy2(self.binary, self.executable)

    def install_assets(self, name="svg_text_interaction.assets", parent=None):
        self.assertIsNotNone(self.assets)
        destination = (parent or self.install) / name
        shutil.copytree(self.assets, destination)
        return destination

    def run_example(self):
        env = dict(os.environ)
        for name in ("BUILD_WORKING_DIRECTORY", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE",
                     "RUNFILES_MANIFEST_ONLY", "TEST_SRCDIR"):
            env.pop(name, None)
        output = self.cwd / "text.png"
        result = subprocess.run([str(self.executable), str(output)], cwd=self.cwd, env=env,
                                capture_output=True, text=True, timeout=30)
        return result, output

    def assert_rendered(self, result, output):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Rendered to ", result.stdout)
        image = output.read_bytes()
        self.assertEqual(image[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack(">II", image[16:24]), (400, 120))


class NativeResourceDeliveryTests(ResourceDeliveryFixture):
    def test_relocated_example_lays_out_text_with_its_offline_companion(self):
        self.install_assets()
        result, output = self.run_example()
        self.assert_rendered(result, output)
        length = re.search(r"Computed text length: ([0-9.]+)", result.stdout)
        self.assertIsNotNone(length, result.stdout)
        self.assertGreater(float(length.group(1)), 0)

    def test_missing_companion_cannot_use_a_valid_tree_in_the_working_directory(self):
        self.install_assets(name="donner-assets", parent=self.cwd)
        result, output = self.run_example()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Packaged resources unavailable:", result.stderr)
        self.assertFalse(output.exists())

    def test_corrupt_named_asset_cannot_fall_back_to_a_valid_generic_tree(self):
        named = self.install_assets()
        self.install_assets(name="donner-assets")
        manifest = json.loads((named / "manifest.json").read_text())
        payload = named / manifest["assets"][0]["path"]
        body = bytearray(payload.read_bytes())
        body[0] ^= 1
        payload.write_bytes(body)
        result, output = self.run_example()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Packaged resources unavailable:", result.stderr)
        self.assertFalse(output.exists())

    def test_corrupt_named_manifest_cannot_fall_back_to_a_valid_generic_tree(self):
        named = self.install_assets()
        self.install_assets(name="donner-assets")
        (named / "manifest.json").write_text("{}\n")
        result, output = self.run_example()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Packaged resources unavailable:", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
