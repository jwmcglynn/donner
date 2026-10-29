"""Relocate actual native executables and qualify only their installed resource roots."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class NativeResourceFixture(unittest.TestCase):
    def setUp(self):
        self.resolver = runfiles.Create()
        self.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR"))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / "relocated"
        self.install.mkdir()
        self.cwd = self.root / "unrelated-cwd"
        self.cwd.mkdir()
        package, name, implementation = self.consumer
        source = Path(self.resolver.Rlocation("donner/" + package + "/" + name))
        self.launcher = source
        if implementation != name:
            source = Path(self.resolver.Rlocation("_donner_transitioned/" + package + "/" + name))
        self.executable = self.install / implementation
        shutil.copy2(source, self.executable)
        self.asset_name = implementation + ".assets"
        assets = self.resolver.Rlocation("donner/" + package + "/" + self.asset_name)
        self.assets = Path(assets) if assets else None

    def install_assets(self, name=None, parent=None):
        self.assertIsNotNone(self.assets, "companion missing from runfiles")
        self.assertTrue(self.assets.is_dir(), "missing declared companion: " + str(self.assets))
        destination = (parent or self.install) / (name or self.asset_name)
        shutil.copytree(self.assets, destination, copy_function=shutil.copyfile)
        return destination

    def run_binary(self, args=(), input="", public_wrapper=False):
        env = dict(os.environ)
        env.pop("BUILD_WORKING_DIRECTORY", None)
        if not public_wrapper:
            for name in ("RUNFILES_DIR", "RUNFILES_MANIFEST_FILE", "RUNFILES_MANIFEST_ONLY",
                         "TEST_SRCDIR"):
                env.pop(name, None)
        binary = self.launcher if public_wrapper else self.executable
        return subprocess.run([str(binary), *map(str, args)], input=input, cwd=self.cwd, env=env,
                              capture_output=True, text=True, timeout=20)

    def svg(self, name, text):
        path = self.cwd / (name + ".svg")
        path.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64">'
                        '<rect width="1" height="1" fill="blue"/>' +
                        ('<text x="4" y="42" font-family="UninstalledFixtureFamily" '
                         'font-size="32" fill="red">Hi</text>' if text else '') + '</svg>')
        return path

    def diagnostics(self, result):
        return result.stderr + result.stdout

    def assert_resource_error(self, result):
        self.assertNotEqual(result.returncode, 0, self.diagnostics(result))
        self.assertIn("Packaged resources unavailable:", self.diagnostics(result))
        self.assertEqual(list(self.cwd.glob("*.png")), [], self.diagnostics(result))

    def record_verified_registry(self, comparison=None):
        output = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR")
        if output:
            manifest = json.loads((self.assets / "manifest.json").read_text())
            record = {"consumer": "/".join(self.consumer[:2]),
                      "contract": "startup preflight accepted this exact compiled resource set",
                      "assetCount": len(manifest["assets"]), "manifest": manifest,
                      "glyphComparison": comparison}
            (Path(output) / (self.consumer[2] + "-registry.json")).write_text(
                json.dumps(record, sort_keys=True) + "\n")

    def assert_glyph_pixels(self):
        self.install_assets()
        fontless = self.render(self.svg("fontless", False), "fontless.png")
        text = self.render(self.svg("text", True), "text.png")
        compare = self.resolver.Rlocation("donner/donner/svg/renderer/tests/svg2_image_compare")
        result = subprocess.run([compare, "--expected", str(fontless), "--actual", str(text)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "ok", report)
        self.assertGreater(report["mismatched_pixels"], 20, report)
        self.record_verified_registry(report)


class DeliveryChecks:
    def test_missing_companion(self):
        self.assert_resource_error(self.invoke())

    def test_valid_tree_in_cwd_cannot_replace_a_missing_companion(self):
        self.install_assets(name="donner-assets", parent=self.cwd)
        self.assert_resource_error(self.invoke())

    def test_corrupt_named_body_cannot_use_a_valid_generic_tree(self):
        named = self.install_assets()
        self.install_assets(name="donner-assets")
        manifest = json.loads((named / "manifest.json").read_text())
        payload = named / manifest["assets"][0]["path"]
        body = bytearray(payload.read_bytes())
        body[0] ^= 1
        payload.write_bytes(body)
        self.assert_resource_error(self.invoke())

    def test_wrong_named_registry_cannot_use_a_valid_generic_tree(self):
        named = self.install_assets()
        self.install_assets(name="donner-assets")
        manifest = named / "manifest.json"
        content = manifest.read_text()
        identity = json.loads(content)["assets"][0]["id"]
        manifest.write_text(content.replace(identity, "WrongCompiledResourceIdentity", 1))
        self.assert_resource_error(self.invoke())
