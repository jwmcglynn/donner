"""Headless render test for the tiny_skia Donner SVG WebAssembly module.

This is the automated safety gate for JS-glue size work (closure minification,
`-sFILESYSTEM=0`, and similar). It loads the built Emscripten module under Node
via `render_test_driver.mjs`, renders a nontrivial SVG through the public C API,
reads the pixels back through the exported `HEAPU8` view, and asserts the render
is non-blank and its pixel hash matches a checked-in golden. A glue regression
(for example closure renaming `cwrap`/`UTF8ToString`/`HEAPU8`) breaks the render
and fails this test; the golden itself is invariant to glue changes because the
pixels come from the wasm module.

Run it against the shipped, size-optimized artifact:

    bazel test --config=wasm-size //donner/svg/renderer/wasm:render_test

The module only loads outside a browser when built with assertions off (opt),
which `--config=wasm-size` provides; the unoptimized `--config=wasm` dev build
keeps an environment guard and is not the gate target. Node is located via
$DONNER_NODE, then PATH, then common install locations; if none is found the
test fails loudly rather than silently passing.
"""

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles

_GLUE_RLOCATION = "donner/donner/svg/renderer/wasm/donner_wasm_bin.js"
_WASM_RLOCATION = "donner/donner/svg/renderer/wasm/donner_wasm_bin.wasm"
_DRIVER_RLOCATION = "donner/donner/svg/renderer/wasm/render_test_driver.mjs"

# FNV-1a hash of the 64x64 RGBA render of the reference SVG in the driver,
# measured on the size-optimized (--config=wasm-size) tiny_skia wasm build.
# Regenerate only on a deliberate renderer change (run the driver and copy the
# HASH= line), never to paper over a glue regression.
_GOLDEN_HASH = "28ece9d9"
_WIDTH = 64
_HEIGHT = 64
_EXPECTED_BYTES = _WIDTH * _HEIGHT * 4


def _find_node():
    candidates = [
        os.environ.get("DONNER_NODE"),
        shutil.which("node"),
        "/opt/homebrew/bin/node",
        "/usr/local/bin/node",
        "/usr/bin/node",
    ]
    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            return candidate
    return None


class WasmRenderTest(unittest.TestCase):
    def setUp(self):
        self.node = _find_node()
        self.assertIsNotNone(
            self.node,
            "node not found; set $DONNER_NODE or put node on PATH "
            "(the headless wasm render gate requires Node).",
        )
        self.r = runfiles.Create()
        self.assertIsNotNone(self.r, "runfiles not available")

    def _rlocation(self, path):
        located = self.r.Rlocation(path)
        self.assertIsNotNone(located, path + " not found in runfiles")
        self.assertTrue(os.path.exists(located), located)
        return located

    def _run_driver(self, glue_path, wasm_path):
        env = os.environ.copy()
        node_options = shlex.split(env.get("NODE_OPTIONS", ""))
        node_options = [option for option in node_options if option != "--jitless"]
        if node_options:
            env["NODE_OPTIONS"] = shlex.join(node_options)
        else:
            env.pop("NODE_OPTIONS", None)

        result = subprocess.run(
            [
                self.node,
                self._rlocation(_DRIVER_RLOCATION),
                glue_path,
                wasm_path,
                str(_WIDTH),
                str(_HEIGHT),
            ],
            capture_output=True,
            env=env,
            text=True,
        )
        return result

    def _parse(self, stdout):
        fields = {}
        for line in stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                fields[key.strip()] = value.strip()
        return fields

    def test_renders_reference_svg(self):
        glue = self._rlocation(_GLUE_RLOCATION)
        wasm = self._rlocation(_WASM_RLOCATION)

        # The Emscripten glue is ES6 (uses `export` / `import.meta`), so Node
        # must see it with a .mjs extension. Copy it next to nothing special;
        # the driver reads the .wasm bytes directly, so co-location is not
        # required.
        with tempfile.TemporaryDirectory() as tmp:
            glue_mjs = os.path.join(tmp, "donner_wasm_bin.mjs")
            shutil.copyfile(glue, glue_mjs)

            result = self._run_driver(glue_mjs, wasm)

        self.assertEqual(
            result.returncode,
            0,
            "render driver failed (exit {}):\nSTDOUT:\n{}\nSTDERR:\n{}".format(
                result.returncode, result.stdout, result.stderr
            ),
        )

        fields = self._parse(result.stdout)
        self.assertIn("HASH", fields, "driver did not print HASH:\n" + result.stdout)
        self.assertIn("BYTES", fields, "driver did not print BYTES:\n" + result.stdout)

        self.assertEqual(
            int(fields["BYTES"]),
            _EXPECTED_BYTES,
            "unexpected pixel byte count",
        )

        nonzero, _, denom = fields.get("NONZERO", "0/0").partition("/")
        nonzero = int(nonzero)
        self.assertGreater(
            nonzero,
            int(_EXPECTED_BYTES * 0.9),
            "rendered image is mostly blank ({} / {} non-zero bytes); "
            "the glue likely failed to marshal the render".format(
                nonzero, _EXPECTED_BYTES
            ),
        )

        self.assertEqual(
            fields["HASH"],
            _GOLDEN_HASH,
            "pixel hash {} != golden {}. If this is a deliberate renderer "
            "change, regenerate the golden; otherwise a glue or rendering "
            "regression changed the output.".format(fields["HASH"], _GOLDEN_HASH),
        )


if __name__ == "__main__":
    unittest.main()
