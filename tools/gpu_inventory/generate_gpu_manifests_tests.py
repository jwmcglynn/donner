"""Unit tests for generate_gpu_manifests.py scanning and rendering logic."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_gpu_manifests as gen


class ScanWgpuTokensTest(unittest.TestCase):
    def test_extracts_cpp_c_and_type_tokens(self):
        text = (
            "wgpu::Device device;\n"
            "WGPUBindGroupLayout layouts[1];\n"
            "wgpuAdapterGetInfo(adapter, &info);\n"
        )
        result = gen.scan_wgpu_tokens(text)
        self.assertEqual(result["wgpuCppTokens"], ["Device"])
        self.assertEqual(result["wgpuCFunctions"], ["wgpuAdapterGetInfo"])
        self.assertEqual(result["wgpuCTypes"], ["WGPUBindGroupLayout"])

    def test_operations_only_recorded_alongside_wgpu_tokens(self):
        self.assertEqual(gen.scan_wgpu_tokens("renderer.draw(document);"), {})

        text = "wgpu::Queue queue;\nqueue.submit(1, &buf);\nencoder.draw(4);\n"
        result = gen.scan_wgpu_tokens(text)
        self.assertEqual(result["operations"], ["draw", "submit"])

    def test_no_tokens_returns_empty(self):
        self.assertEqual(gen.scan_wgpu_tokens("int main() { return 0; }"), {})


class ScanWgslTest(unittest.TestCase):
    WGSL = """
struct Uniforms {
  mvp: mat4x4f,
  color: vec4f,
};
@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> curveData: array<f32>;
@group(0) @binding(2) var patternTexture: texture_2d<f32>;

@vertex
fn vs_main(@builtin(instance_index) idx: u32) -> @builtin(position) vec4f {
  return uniforms.mvp * vec4f(0.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
  for (var i = 0u; i < 4u; i = i + 1u) { }
  discard;
}

@compute @workgroup_size(8, 8)
fn main() { }
"""

    def test_entry_points_sorted_by_stage_then_name(self):
        result = gen.scan_wgsl(self.WGSL)
        self.assertEqual(
            result["entryPoints"],
            [
                {"name": "main", "stage": "compute", "workgroupSize": ["8", "8"]},
                {"name": "fs_main", "stage": "fragment"},
                {"name": "vs_main", "stage": "vertex"},
            ],
        )

    def test_bindings_include_address_space_and_type(self):
        result = gen.scan_wgsl(self.WGSL)
        self.assertEqual(
            result["bindings"],
            [
                {
                    "group": 0,
                    "binding": 0,
                    "name": "uniforms",
                    "type": "Uniforms",
                    "addressSpace": "uniform",
                },
                {
                    "group": 0,
                    "binding": 1,
                    "name": "curveData",
                    "type": "array<f32>",
                    "addressSpace": "storage, read",
                },
                {
                    "group": 0,
                    "binding": 2,
                    "name": "patternTexture",
                    "type": "texture_2d<f32>",
                },
            ],
        )

    def test_features_builtins_and_structs(self):
        result = gen.scan_wgsl(self.WGSL)
        self.assertIn("discard", result["features"])
        self.assertIn("for", result["features"])
        self.assertIn("mat4x4f", result["features"])
        self.assertNotIn("textureSample", result["features"])
        self.assertEqual(result["builtins"], ["instance_index", "position"])
        self.assertEqual(result["structs"], ["Uniforms"])


class BuildManifestsTest(unittest.TestCase):
    FILES = {
        "donner/gpu_user.cc": "wgpu::Device d;\nd.createBuffer(desc);\n",
        "donner/editor/Panel.cc": "ImGui_ImplWGPU_NewFrame();\n#ifdef DONNER_EDITOR_WGPU\n#endif\n",
        "donner/shaders/fill.wgsl": "@fragment\nfn fs_main() { }\n",
        "donner/inline_shader.cc": 'constexpr std::string_view kWgsl = R"wgsl(@vertex\nfn vs() {})wgsl";\n',
        "donner/plain.cc": "int main() { return 0; }\n",
        "examples/outside.cc": "wgpu::Device d;\n",
        "third_party/vendored/lib.rs": "fn main() {}\n",
        "third_party/active/Cargo.toml": "[package]\n",
        "MODULE.bazel": 'bazel_dep(name = "rules_rust", version = "0.1")\n',
        "third_party/deps.bzl": "url = 'gfx-rs/wgpu-native'\n_WGPU_NATIVE_VERSION = \"v1.2.3\"\n",
    }
    ALLOWLIST = ["third_party/vendored/"]

    def test_gpu_operations_scoped_to_donner_sources(self):
        manifest = gen.build_gpu_operations_manifest(self.FILES)
        self.assertEqual(list(manifest["files"]), ["donner/gpu_user.cc"])
        self.assertEqual(manifest["files"]["donner/gpu_user.cc"]["operations"], ["createBuffer"])

    def test_shader_manifest_includes_inline_wgsl(self):
        manifest = gen.build_shader_features_manifest(self.FILES)
        self.assertEqual(
            sorted(manifest["shaders"]),
            ["donner/inline_shader.cc#kWgsl", "donner/shaders/fill.wgsl"],
        )

    def test_editor_manifest_captures_imgui_and_define(self):
        manifest = gen.build_editor_integration_manifest(self.FILES)
        entry = manifest["files"]["donner/editor/Panel.cc"]
        self.assertEqual(entry["imguiWgpuTokens"], ["ImGui_ImplWGPU_NewFrame"])
        self.assertTrue(entry["usesEditorWgpuDefine"])

    def test_rust_manifest_partitions_allowlisted_and_active(self):
        manifest = gen.build_rust_dependencies_manifest(self.FILES, self.ALLOWLIST)
        self.assertEqual(manifest["allowlistedRustSources"], ["third_party/vendored/lib.rs"])
        self.assertEqual(manifest["activeRustSources"], ["third_party/active/Cargo.toml"])
        self.assertEqual(manifest["rustBuildEdges"], {"MODULE.bazel": ["rules_rust"]})
        self.assertEqual(
            manifest["rustBuiltArchiveSites"],
            {
                "third_party/deps.bzl": {
                    "tokens": ["gfx-rs/wgpu-native"],
                    "wgpuNativeVersion": "v1.2.3",
                }
            },
        )

    def test_build_all_manifests_is_deterministic(self):
        first = gen.build_all_manifests(self.FILES, self.ALLOWLIST)
        second = gen.build_all_manifests(dict(reversed(list(self.FILES.items()))), self.ALLOWLIST)
        self.assertEqual(
            {n: gen.render_manifest(m) for n, m in first.items()},
            {n: gen.render_manifest(m) for n, m in second.items()},
        )

    def test_render_manifest_ends_with_single_newline(self):
        rendered = gen.render_manifest({"a": 1})
        self.assertTrue(rendered.endswith("}\n"))
        self.assertFalse(rendered.endswith("\n\n"))


if __name__ == "__main__":
    unittest.main()
