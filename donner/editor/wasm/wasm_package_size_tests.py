#!/usr/bin/env python3
"""Deterministic compressed-size budgets for a shipped Wasm module and its JavaScript glue.

The editor package passes `--package-dir`; a standalone module passes `--wasm-file` and `--js-file`.
Budgets that do not apply to a module (the package total, the function-body and data-segment
complexity caps) are omitted from its arguments, and their checks are skipped.
Report mode still checks the artifact contracts and reports every size and complexity metric
without enforcing its numeric ceiling.
"""

from __future__ import annotations

import argparse
import contextlib
import gzip
import io
import tempfile
import unittest
from pathlib import Path


def _compressed_size(path: Path) -> int:
    return len(gzip.compress(path.read_bytes(), compresslevel=9, mtime=0))


def _package_raw_size(package_dir: Path) -> int:
    """Return bytes shipped on disk, independent of the Wasm runtime heap size."""
    return sum(path.stat().st_size for path in package_dir.rglob("*") if path.is_file())


def _check_payload_budget(
    test: unittest.TestCase,
    metric: str,
    actual: int,
    limit: int,
    message: str,
    prefix: str = "editor",
    enforce: bool = True,
) -> None:
    print(f"{prefix}-payload-budget metric={metric} actual={actual} limit={limit} "
          f"delta={actual - limit}")
    if enforce:
        test.assertLessEqual(actual, limit, message)


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
    def test_payload_budget_rejects_overage_after_reporting_it(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(AssertionError):
            _check_payload_budget(self, "fixture", 101, 100, "fixture exceeded")
        self.assertIn("metric=fixture actual=101 limit=100 delta=1", output.getvalue())

    def test_payload_budget_accepts_the_ceiling(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            _check_payload_budget(self, "fixture", 100, 100, "fixture exceeded")
        self.assertIn("metric=fixture actual=100 limit=100 delta=0", output.getvalue())

    def test_payload_budget_reports_the_module_prefix(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            _check_payload_budget(self, "fixture", 100, 100, "fixture exceeded", "renderer")
        self.assertIn("renderer-payload-budget metric=fixture actual=100", output.getvalue())

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
    payload_budget_mode: str = "enforce"
    package_dir: Path | None
    wasm_path: Path
    js_path: Path
    prefix: str
    max_wasm_raw_bytes: int
    max_wasm_gzip_bytes: int
    max_wasm_function_body_bytes: int | None
    max_wasm_data_segments: int | None
    max_js_raw_bytes: int
    max_js_gzip_bytes: int
    max_total_raw_bytes: int | None
    expected_js_properties: list[str]
    forbidden_js_tokens: list[str]

    def test_total_package_fits_raw_size_budget(self) -> None:
        if self.package_dir is None or self.max_total_raw_bytes is None:
            self.skipTest("no package directory budget for this module")
        total_raw_bytes = _package_raw_size(self.package_dir)
        print(f"{self.prefix}-package-total-size raw={total_raw_bytes}")
        _check_payload_budget(
            self,
            "total_raw",
            total_raw_bytes,
            self.max_total_raw_bytes,
            f"{self.prefix} package raw size exceeded its production budget",
            self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )

    def test_javascript_bridge_contract_names_survive_minification(self) -> None:
        javascript = self.js_path.read_text(encoding="utf-8")
        for property_name in self.expected_js_properties:
            self.assertIn(
                property_name,
                javascript,
                f"JavaScript minification renamed the cross-language bridge {property_name!r}",
            )

    def test_javascript_omits_backend_incompatible_runtime_support(self) -> None:
        javascript = self.js_path.read_text(encoding="utf-8")
        for token in self.forbidden_js_tokens:
            self.assertFalse(
                token in javascript,
                f"backend-incompatible JavaScript runtime support remains: {token!r}",
            )

    def test_shipped_payload_fits_compressed_budgets(self) -> None:
        wasm_path = self.wasm_path
        js_path = self.js_path
        self.assertTrue(wasm_path.is_file(), f"missing shipped Wasm module: {wasm_path}")
        self.assertTrue(js_path.is_file(), f"missing shipped JavaScript glue: {js_path}")

        wasm_gzip_bytes = _compressed_size(wasm_path)
        js_gzip_bytes = _compressed_size(js_path)
        print(
            f"{self.prefix}-package-size "
            f"wasm_raw={wasm_path.stat().st_size} wasm_gzip={wasm_gzip_bytes} "
            f"js_raw={js_path.stat().st_size} js_gzip={js_gzip_bytes}"
        )
        _check_payload_budget(
            self,
            "wasm_raw",
            wasm_path.stat().st_size,
            self.max_wasm_raw_bytes,
            "Wasm module raw decode/compile size exceeded its production budget",
            self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )
        _check_payload_budget(
            self,
            "wasm_gzip",
            wasm_gzip_bytes,
            self.max_wasm_gzip_bytes,
            "Wasm module compressed transfer size exceeded its production budget",
            self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )
        _check_payload_budget(
            self,
            "js_raw",
            js_path.stat().st_size,
            self.max_js_raw_bytes,
            "JavaScript glue raw parse size exceeded its production budget",
            self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )
        _check_payload_budget(
            self,
            "js_gzip",
            js_gzip_bytes,
            self.max_js_gzip_bytes,
            "JavaScript glue compressed transfer size exceeded its production budget",
            self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )

    def test_wasm_functions_fit_browser_tier_compiler_budget(self) -> None:
        if self.max_wasm_function_body_bytes is None:
            self.skipTest("no function-body budget for this module")
        function_body_sizes = _wasm_function_body_sizes(self.wasm_path)
        largest_body_bytes = max(function_body_sizes, default=0)
        print(
            f"{self.prefix}-wasm-function-size "
            f"largest_body={largest_body_bytes} function_count={len(function_body_sizes)}"
        )
        _check_payload_budget(
            self, "wasm_function_body", largest_body_bytes, self.max_wasm_function_body_bytes,
            "one Wasm function exceeds the browser tier-compiler complexity budget", self.prefix,
            enforce=self.payload_budget_mode == "enforce",
        )

    def test_static_memory_initializer_fits_browser_tier_compiler_budget(self) -> None:
        if self.max_wasm_data_segments is None:
            self.skipTest("no data-segment budget for this module")
        data_segment_count = _wasm_section_vector_count(self.wasm_path, 11)
        print(f"{self.prefix}-wasm-data-segments count={data_segment_count}")
        _check_payload_budget(
            self, "wasm_data_segments", data_segment_count, self.max_wasm_data_segments,
            "the Wasm module has enough passive data segments to create a pathological memory "
            "initializer", self.prefix, enforce=self.payload_budget_mode == "enforce",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=Path)
    parser.add_argument("--wasm-file", type=Path)
    parser.add_argument("--js-file", type=Path)
    parser.add_argument("--metric-prefix", default="editor")
    parser.add_argument("--payload-budget-mode", choices=("enforce", "report"), default="enforce")
    parser.add_argument("--max-wasm-raw-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-wasm-function-body-bytes", type=int)
    parser.add_argument("--max-wasm-data-segments", type=int)
    parser.add_argument("--max-js-raw-bytes", type=int, required=True)
    parser.add_argument("--max-js-gzip-bytes", type=int, required=True)
    parser.add_argument("--max-total-raw-bytes", type=int)
    parser.add_argument("--expected-js-property", action="append", default=[])
    parser.add_argument("--forbidden-js-token", action="append", default=[])
    args, unittest_args = parser.parse_known_args()
    if args.package_dir is None and (args.wasm_file is None or args.js_file is None):
        parser.error("pass --package-dir, or both --wasm-file and --js-file")
    WasmPackageSizeTest.payload_budget_mode = args.payload_budget_mode
    WasmPackageSizeTest.package_dir = args.package_dir
    WasmPackageSizeTest.wasm_path = args.wasm_file or args.package_dir / "editor.wasm"
    WasmPackageSizeTest.js_path = args.js_file or args.package_dir / "editor.js"
    WasmPackageSizeTest.prefix = args.metric_prefix
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
