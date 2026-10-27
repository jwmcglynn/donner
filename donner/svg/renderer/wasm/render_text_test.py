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
        glue = resolver.Rlocation(base + "donner_wasm_bin.js")
        wasm = resolver.Rlocation(base + "donner_wasm_bin.wasm")
        driver = resolver.Rlocation(base + "render_text_test_driver.mjs")
        assets = resolver.Rlocation("donner/third_party/public-sans/public-sans_resource_gen.assets")
        broker = resolver.Rlocation("donner/donner/editor/wasm/catalog-fonts.js")
        loader = resolver.Rlocation("donner/donner/editor/wasm/packaged-resource-loader.js")
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
