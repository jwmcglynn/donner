#!/usr/bin/env python3
"""Deterministic compressed-size budgets for a shipped editor Wasm package."""

from __future__ import annotations

import argparse
import gzip
import tempfile
import unittest
from pathlib import Path


def _compressed_size(path: Path) -> int:
    return len(gzip.compress(path.read_bytes(), compresslevel=9, mtime=0))


def _package_raw_size(package_dir: Path) -> int:
    """Return bytes shipped on disk, independent of the Wasm runtime heap size."""
    return sum(path.stat().st_size for path in package_dir.rglob("*") if path.is_file())


class PackageSizeAccountingTest(unittest.TestCase):
    def test_runtime_memory_reservation_is_not_counted_as_download_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            package_dir = Path(directory)
            javascript = b"-sINITIAL_MEMORY=64MB"
            wasm = b"\x00asm"
            (package_dir / "editor.js").write_bytes(javascript)
            (package_dir / "editor.wasm").write_bytes(wasm)

            self.assertEqual(_package_raw_size(package_dir), len(javascript) + len(wasm))
            self.assertLess(_package_raw_size(package_dir), 64 * 1024 * 1024)


class WasmPackageSizeTest(unittest.TestCase):
    package_dir: Path
    max_wasm_raw_bytes: int
    max_wasm_gzip_bytes: int
    max_js_raw_bytes: int
    max_js_gzip_bytes: int
    max_total_raw_bytes: int
    expected_js_properties: list[str]
    forbidden_js_tokens: list[str]

    def test_total_package_fits_raw_size_budget(self) -> None:
        total_raw_bytes = _package_raw_size(self.package_dir)
        print(f"editor-package-total-size raw={total_raw_bytes}")
        self.assertLessEqual(
            total_raw_bytes,
            self.max_total_raw_bytes,
            "editor package raw size exceeded its production budget",
        )

    def test_javascript_bridge_contract_names_survive_minification(self) -> None:
        editor_js = (self.package_dir / "editor.js").read_text(encoding="utf-8")
        for property_name in self.expected_js_properties:
            self.assertIn(
                property_name,
                editor_js,
                f"JavaScript minification renamed the cross-language bridge {property_name!r}",
            )

    def test_javascript_omits_backend_incompatible_runtime_support(self) -> None:
        editor_js = (self.package_dir / "editor.js").read_text(encoding="utf-8")
        for token in self.forbidden_js_tokens:
            self.assertFalse(
                token in editor_js,
                f"backend-incompatible JavaScript runtime support remains: {token!r}",
            )

    def test_shipped_editor_payload_fits_compressed_budgets(self) -> None:
        wasm_path = self.package_dir / "editor.wasm"
        js_path = self.package_dir / "editor.js"
        self.assertTrue(wasm_path.is_file(), f"missing shipped Wasm module: {wasm_path}")
        self.assertTrue(js_path.is_file(), f"missing shipped JavaScript glue: {js_path}")

        wasm_gzip_bytes = _compressed_size(wasm_path)
        js_gzip_bytes = _compressed_size(js_path)
        print(
            "editor-package-size "
            f"wasm_raw={wasm_path.stat().st_size} wasm_gzip={wasm_gzip_bytes} "
            f"js_raw={js_path.stat().st_size} js_gzip={js_gzip_bytes}"
        )
        self.assertLessEqual(
            wasm_path.stat().st_size,
            self.max_wasm_raw_bytes,
            "editor.wasm raw decode/compile size exceeded its production budget",
        )
        self.assertLessEqual(
            wasm_gzip_bytes,
            self.max_wasm_gzip_bytes,
            "editor.wasm compressed transfer size exceeded its production budget",
        )
        self.assertLessEqual(
            js_path.stat().st_size,
            self.max_js_raw_bytes,
            "editor.js raw parse size exceeded its production budget",
        )
        self.assertLessEqual(
            js_gzip_bytes,
            self.max_js_gzip_bytes,
            "editor.js compressed transfer size exceeded its production budget",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--max-wasm-raw-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-js-raw-bytes", type=int, required=True)
    parser.add_argument("--max-js-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-total-raw-bytes", type=int, required=True)
    parser.add_argument("--expected-js-property", action="append", default=[])
    parser.add_argument("--forbidden-js-token", action="append", default=[])
    args, unittest_args = parser.parse_known_args()
    WasmPackageSizeTest.package_dir = args.package_dir
    WasmPackageSizeTest.max_wasm_raw_bytes = args.max_wasm_raw_bytes
    WasmPackageSizeTest.max_wasm_gzip_bytes = args.max_wasm_gzip_bytes
    WasmPackageSizeTest.max_js_raw_bytes = args.max_js_raw_bytes
    WasmPackageSizeTest.max_js_gzip_bytes = args.max_js_gzip_bytes
    WasmPackageSizeTest.max_total_raw_bytes = args.max_total_raw_bytes
    WasmPackageSizeTest.expected_js_properties = args.expected_js_property
    WasmPackageSizeTest.forbidden_js_tokens = args.forbidden_js_token
    unittest.main(argv=[__file__, *unittest_args])
