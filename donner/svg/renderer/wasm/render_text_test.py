"""Exercise actual text-enabled standalone Wasm with authenticated host-owned font bytes."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class WasmTextRenderTest(unittest.TestCase):
    def run_case(self, case):
        resolver = runfiles.Create()
        node = os.environ["DONNER_NODE_PATH"]
        self.assertTrue(node.startswith("external/"))
        node = resolver.Rlocation(node[len("external/"):])
        base = "donner/donner/svg/renderer/wasm/"
        package = Path(resolver.Rlocation(base + "test_page"))
        glue = package / "donner_wasm_bin.js"
        wasm = str(package / "donner_wasm_bin.wasm")
        driver = resolver.Rlocation(base + "render_text_test_driver.mjs")
        assets = str(package)
        broker = str(package / "catalog-fonts.js")
        loader = str(package / "packaged-resource-loader.js")
        notice = (package / "fallback_font_notice.txt").read_text()
        self.assertIn("Public Sans", notice)
        self.assertIn("SIL OPEN FONT LICENSE", notice)
        env = dict(os.environ)
        options = [value for value in shlex.split(env.get("NODE_OPTIONS", "")) if value != "--jitless"]
        env["NODE_OPTIONS"] = shlex.join(options)
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as temporary:
            module = Path(temporary) / "renderer.mjs"
            shutil.copyfile(glue, module)
            result = subprocess.run([node, driver, str(module), wasm, assets, broker, loader, case],
                                    env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS=" + case, result.stdout)

    def test_missing_fallback_reports_a_resource_error(self):
        self.run_case("missing")

    def test_installed_host_owned_font_produces_text_pixels(self):
        self.run_case("installed")

    def test_corrupt_host_owned_font_is_rejected(self):
        self.run_case("corrupt")

    def test_wrong_package_manifest_is_rejected(self):
        self.run_case("manifest-mismatch")


if __name__ == "__main__":
    unittest.main()
