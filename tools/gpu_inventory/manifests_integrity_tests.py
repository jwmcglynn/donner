"""Structural integrity tests for the checked-in GPU inventory manifests.

Whole-tree freshness is enforced by the Lint workflow's
`generate_gpu_manifests.py --check` step (a sandboxed bazel test cannot see the
full source tree). These tests assert that the checked-in manifests are
well-formed and still describe the invariants later design 0053 packets rely on.
"""

from __future__ import annotations

import json
import unittest

from python.runfiles import runfiles

MANIFEST_DIR = "donner/tools/gpu_inventory/manifests"


def load_manifest(name: str) -> dict:
    r = runfiles.Create()
    path = r.Rlocation(f"{MANIFEST_DIR}/{name}")
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


class GpuOperationsManifestTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest("gpu_operations.json")

    def test_has_files_with_tokens(self):
        files = self.manifest["files"]
        self.assertGreater(len(files), 0)
        for path, entry in files.items():
            self.assertTrue(path.startswith("donner/"), path)
            self.assertTrue(
                any(k in entry for k in ("wgpuCppTokens", "wgpuCFunctions", "wgpuCTypes")),
                f"{path} has no wgpu tokens",
            )

    def test_core_geode_files_present(self):
        files = self.manifest["files"]
        self.assertIn("donner/svg/renderer/geode/GeodeDevice.cc", files)
        self.assertIn("donner/svg/renderer/RendererGeode.cc", files)


class ShaderFeaturesManifestTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest("shader_features.json")

    def test_solid_fill_shader_shape(self):
        shader = self.manifest["shaders"]["donner/svg/renderer/geode/shaders/slug_fill.wgsl"]
        stages = sorted(e["stage"] for e in shader["entryPoints"])
        self.assertEqual(stages, ["fragment", "vertex"])
        self.assertEqual(len(shader["bindings"]), 12)

    def test_every_shader_has_an_entry_point(self):
        for name, shader in self.manifest["shaders"].items():
            self.assertGreater(len(shader["entryPoints"]), 0, name)


class RustDependenciesManifestTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_manifest("rust_dependencies.json")

    def test_allowlist_prefixes_match_verifier_allowlist(self):
        r = runfiles.Create()
        allowlist_path = r.Rlocation("donner/tools/gpu_inventory/rust_allowlist.json")
        with open(allowlist_path, encoding="utf-8") as handle:
            allowlist = json.load(handle)["inertReferencePrefixes"]
        self.assertEqual(self.manifest["allowlistPrefixes"], sorted(allowlist))

    def test_active_rust_sources_do_not_grow(self):
        # Ratchet: the only active (non-allowlisted) Rust material is the
        # tiny-skia FFI fixture, which design 0053 phase 6 removes. Any new
        # entry here is a regression against the no-Rust invariant.
        for path in self.manifest["activeRustSources"]:
            self.assertTrue(
                path.startswith("third_party/tiny-skia-cpp/tests/rust_ffi/"),
                f"unexpected active Rust source: {path}",
            )


if __name__ == "__main__":
    unittest.main()
