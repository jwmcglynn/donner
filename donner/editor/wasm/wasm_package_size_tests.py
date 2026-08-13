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


def _decode_u32_leb(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 35, 7):
        if offset >= len(data):
            raise ValueError("truncated unsigned LEB128 value")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
    raise ValueError("unsigned LEB128 value exceeds 32 bits")


def _wasm_function_body_sizes(path: Path) -> list[int]:
    data = path.read_bytes()
    if data[:8] != b"\x00asm\x01\x00\x00\x00":
        raise ValueError(f"invalid WebAssembly header: {path}")

    offset = 8
    while offset < len(data):
        section_id = data[offset]
        section_size, payload_offset = _decode_u32_leb(data, offset + 1)
        section_end = payload_offset + section_size
        if section_end > len(data):
            raise ValueError(f"truncated WebAssembly section {section_id}: {path}")
        if section_id == 10:
            function_count, body_offset = _decode_u32_leb(data, payload_offset)
            body_sizes = []
            for _ in range(function_count):
                body_size, body_offset = _decode_u32_leb(data, body_offset)
                body_sizes.append(body_size)
                body_offset += body_size
                if body_offset > section_end:
                    raise ValueError(f"truncated WebAssembly function body: {path}")
            if body_offset != section_end:
                raise ValueError(f"unexpected trailing WebAssembly code bytes: {path}")
            return body_sizes
        offset = section_end

    raise ValueError(f"WebAssembly code section is missing: {path}")


def _wasm_section_vector_count(path: Path, expected_section_id: int) -> int:
    data = path.read_bytes()
    if data[:8] != b"\x00asm\x01\x00\x00\x00":
        raise ValueError(f"invalid WebAssembly header: {path}")

    offset = 8
    while offset < len(data):
        section_id = data[offset]
        section_size, payload_offset = _decode_u32_leb(data, offset + 1)
        section_end = payload_offset + section_size
        if section_end > len(data):
            raise ValueError(f"truncated WebAssembly section {section_id}: {path}")
        if section_id == expected_section_id:
            count, _ = _decode_u32_leb(data, payload_offset)
            return count
        offset = section_end

    return 0


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

    def test_function_body_sizes_are_read_from_the_code_section(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wasm_path = Path(directory) / "tiny.wasm"
            wasm_path.write_bytes(b"\x00asm\x01\x00\x00\x00\x0a\x04\x01\x02\x00\x0b")
            self.assertEqual(_wasm_function_body_sizes(wasm_path), [2])

    def test_section_vector_count_is_read_from_the_requested_section(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wasm_path = Path(directory) / "tiny.wasm"
            wasm_path.write_bytes(b"\x00asm\x01\x00\x00\x00\x0b\x07\x02\x01\x01a\x01\x01b")
            self.assertEqual(_wasm_section_vector_count(wasm_path, 11), 2)
            self.assertEqual(_wasm_section_vector_count(wasm_path, 12), 0)


class WasmPackageSizeTest(unittest.TestCase):
    package_dir: Path
    max_wasm_raw_bytes: int
    max_wasm_gzip_bytes: int
    max_wasm_function_body_bytes: int
    max_wasm_data_segments: int
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

    def test_wasm_functions_fit_browser_tier_compiler_budget(self) -> None:
        wasm_path = self.package_dir / "editor.wasm"
        function_body_sizes = _wasm_function_body_sizes(wasm_path)
        largest_body_bytes = max(function_body_sizes, default=0)
        print(
            "editor-wasm-function-size "
            f"largest_body={largest_body_bytes} function_count={len(function_body_sizes)}"
        )
        self.assertLessEqual(
            largest_body_bytes,
            self.max_wasm_function_body_bytes,
            "one editor.wasm function exceeds the browser tier-compiler complexity budget",
        )

    def test_static_memory_initializer_fits_browser_tier_compiler_budget(self) -> None:
        wasm_path = self.package_dir / "editor.wasm"
        data_segment_count = _wasm_section_vector_count(wasm_path, 11)
        print(f"editor-wasm-data-segments count={data_segment_count}")
        self.assertLessEqual(
            data_segment_count,
            self.max_wasm_data_segments,
            "editor.wasm has enough passive data segments to create a pathological memory initializer",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--max-wasm-raw-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-function-body-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-data-segments", type=int, required=True)
    parser.add_argument("--max-js-raw-bytes", type=int, required=True)
    parser.add_argument("--max-js-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-total-raw-bytes", type=int, required=True)
    parser.add_argument("--expected-js-property", action="append", default=[])
    parser.add_argument("--forbidden-js-token", action="append", default=[])
    args, unittest_args = parser.parse_known_args()
    WasmPackageSizeTest.package_dir = args.package_dir
    WasmPackageSizeTest.max_wasm_raw_bytes = args.max_wasm_raw_bytes
    WasmPackageSizeTest.max_wasm_gzip_bytes = args.max_wasm_gzip_bytes
    WasmPackageSizeTest.max_wasm_function_body_bytes = args.max_wasm_function_body_bytes
    WasmPackageSizeTest.max_wasm_data_segments = args.max_wasm_data_segments
    WasmPackageSizeTest.max_js_raw_bytes = args.max_js_raw_bytes
    WasmPackageSizeTest.max_js_gzip_bytes = args.max_js_gzip_bytes
    WasmPackageSizeTest.max_total_raw_bytes = args.max_total_raw_bytes
    WasmPackageSizeTest.expected_js_properties = args.expected_js_property
    WasmPackageSizeTest.forbidden_js_tokens = args.forbidden_js_token
    unittest.main(argv=[__file__, *unittest_args])
