"""Negative controls for configured product, receipt, and package boundaries."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import configured_rust_closure as gate


class ConfiguredRustClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = gate.inventory()

    def test_product_to_oracle_edge_is_rejected(self) -> None:
        root = "//donner/editor:editor"
        labels = {root, self.spec["oracleLabel"], "//third_party/webgpu-cpp:webgpu_cpp"}
        with self.assertRaisesRegex(gate.GateError, "test oracle, WebGPU-C\\+\\+ wrapper"):
            gate.check_closure(root, "native", labels, self.spec)

    def test_product_to_archive_edge_is_rejected(self) -> None:
        root = "//donner/svg/renderer/geode:geode_device"
        labels = {root, "@@+non_bcr_deps+wgpu_native_linux_x86_64//:wgpu_native"}
        with self.assertRaisesRegex(gate.GateError, "Rust archive"):
            gate.check_closure(root, "native", labels, self.spec)

    def test_browser_product_to_emscripten_wrapper_is_rejected(self) -> None:
        root = "//donner/editor/wasm:wasm_web_package"
        with self.assertRaisesRegex(gate.GateError, "WebGPU-C\\+\\+ wrapper"):
            gate.check_closure(root, "browser",
                               {root, "//third_party/webgpu-cpp:wgpu_emscripten"}, self.spec)

    def test_oracle_cannot_be_empty_or_used_on_macos(self) -> None:
        with self.assertRaises(gate.GateError):
            gate.check_closure(self.spec["oracleLabel"], "oracle", {self.spec["oracleLabel"]}, self.spec)
        self.assertEqual(self.spec["platforms"]["macos"]["oracle"], [])

    def test_configured_query_must_return_root(self) -> None:
        with self.assertRaisesRegex(gate.GateError, "missing or incompatible"):
            gate.parse_cquery_labels("//other:target\n", "//product:binary")
        with self.assertRaisesRegex(gate.GateError, "empty or incompatible"):
            gate.parse_cquery_labels("//product:binary (config)\n", "//product:binary")

    def test_geode_query_selects_the_enabled_backend(self) -> None:
        root = "//donner/svg/renderer:renderer_geode"
        with patch.object(gate, "run", return_value=f"{root} (cfg)\n//donner/gpu:gpu (cfg)\n") as query:
            gate.query_closure(root, "nativeGeode", "bazel")
            self.assertIn("--config=geode", query.call_args.args)

    def test_required_product_root_cannot_be_removed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            modified = json.loads(json.dumps(self.spec))
            modified["platforms"]["linux"]["native"].remove("//donner/editor:editor")
            path = Path(temp) / "roots.json"
            path.write_text(json.dumps(modified))
            with patch.object(gate, "INVENTORY", path):
                with self.assertRaisesRegex(gate.GateError, "required product"):
                    gate.inventory()

    def test_lock_checks_each_pin_in_its_own_repo_spec(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            lock = Path(temp) / "MODULE.bazel.lock"
            lock.write_text(json.dumps({"moduleExtensions": {
                "//third_party:bazel/non_bcr_deps.bzl%non_bcr_deps": {"general": {"generatedRepoSpecs": {
                    "wgpu_native_linux_x86_64": {"attributes": {"sha256": "b" * 64}},
                }}}
            }}))
            with patch.object(gate, "LOCK", lock):
                with self.assertRaisesRegex(gate.GateError, "wgpu_native_linux_x86_64"):
                    gate.verify_lock({"wgpu_native_linux_x86_64": "a" * 64})

    def test_cmake_absence_is_explicit_and_new_install_rule_fails(self) -> None:
        generated = {"CMakeLists.txt": "add_library(donner INTERFACE)\n",
                     "donner/svg/CMakeLists.txt": "add_library(svg STATIC Svg.cc)\n"}
        self.assertEqual(len(gate.check_generated_cmake_texts(generated)), 64)
        with self.assertRaisesRegex(gate.GateError, "install rules need artifact scanning"):
            gate.check_generated_cmake_texts({**generated, "CMakeLists.txt": "install(TARGETS donner)\n"})
        with self.assertRaisesRegex(gate.GateError, "missing or incomplete"):
            gate.check_generated_cmake_texts({})

    def test_shipped_artifact_scan_rejects_wrapper_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "editor.wasm").write_bytes(b"\0asm\x01\0\0\0")
            (root / "editor.js").write_bytes(b"const editor = true;")
            self.assertEqual(len(gate.scan_artifact_output(root, "//editor:package")), 2)
            (root / "libwgpu_native.so").write_bytes(b"binary")
            with self.assertRaisesRegex(gate.GateError, "Rust-backed GPU path"):
                gate.scan_artifact_output(root, "//editor:package")

    def test_missing_or_stale_platform_receipt_is_rejected(self) -> None:
        identity = {"commit": "a" * 40, "tree": "b" * 40, "inventorySha256": "c" * 64}
        pins = {"wgpu_native_linux_x86_64": "d" * 64}
        rows = {os_name: [
            {"profile": profile, "root": root, "labelsSha256": "e" * 64, "labelCount": 2}
            for profile, roots in self.spec["platforms"][os_name].items() for root in roots
        ] for os_name in ("linux", "macos")}
        artifacts = {os_name: [
            {"profile": profile, "root": root, "files": [
                {"path": name, "bytes": 1, "sha256": "0" * 64}
                for name in (["editor.wasm", "editor.js"] if profile == "browser" else ["donner-svg"])
            ]}
            for profile, roots in self.spec["artifactRoots"][os_name].items() for root in roots
        ] for os_name in ("linux", "macos")}
        receipts = [{"schema": gate.SCHEMA, **identity, "platform": os_name, "architecture": "x86_64",
                     "pins": pins, "lockSha256": "f" * 64 if os_name == "linux" else None, "closures": rows[os_name],
                     "cmake": {"installSurface": "absent", "consumerPassed": True,
                               "generatedCmakeSha256": "1" * 64} if os_name == "linux" else None,
                     "artifacts": artifacts[os_name]}
                    for os_name in ("linux", "macos")]
        with patch.object(gate, "source_identity", return_value=identity), \
             patch.object(gate, "archive_pins", return_value=pins), \
             patch.object(gate, "verify_lock", return_value="f" * 64):
            gate.verify_receipts(receipts)
            with self.assertRaisesRegex(gate.GateError, "both Linux and macOS"):
                gate.verify_receipts(receipts[:1])
            receipts[1]["tree"] = "0" * 40
            with self.assertRaisesRegex(gate.GateError, "stale or mismatched"):
                gate.verify_receipts(receipts)
            receipts[1]["tree"] = identity["tree"]
            receipts[0]["artifacts"][0]["files"][0]["path"] = "libwgpu_native.so"
            with self.assertRaisesRegex(gate.GateError, "contaminated shipped artifact"):
                gate.verify_receipts(receipts)
            receipts[0]["artifacts"][0]["files"][0]["path"] = "donner-svg"
            receipts[0]["artifacts"][0]["files"][0]["sha256"] = 7
            with self.assertRaisesRegex(gate.GateError, "malformed or contaminated"):
                gate.verify_receipts(receipts)
            receipts[0]["artifacts"][0]["files"][0]["sha256"] = "0" * 64
            receipts[0]["closures"].pop()
            with self.assertRaisesRegex(gate.GateError, "missing, duplicate, or unexpected"):
                gate.verify_receipts(receipts)


if __name__ == "__main__":
    unittest.main()
