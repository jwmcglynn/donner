#!/usr/bin/env python3
"""Small negative tests for catalog generation and table preservation checks."""

from pathlib import Path
import json
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from catalog_generate import (
    catalog_memory_budget,
    codec_fingerprint,
    encoder_provenance,
    generate,
    generate_font,
    MAX_FONT_BYTES,
    read_bounded,
    read_catalog_request,
    sfnt_tables,
    verify_preserved_tables,
    validate_encoded_font,
    woff2_workspace,
    sha256,
)


def font(tables):
    """Make a small sfnt directory for independent parser boundary tests."""
    offset = 12 + 16 * len(tables)
    header = bytearray(struct.pack(">IHHHH", 0x10000, len(tables), 0, 0, 0))
    body = bytearray()
    for tag, data in tables.items():
        header.extend(struct.pack(">4sIII", tag, 0, offset, len(data)))
        body.extend(data)
        offset += len(data)
    return bytes(header + body)


def source_tables():
    return {b"head": bytes(54), b"glyf": b"outlines", b"loca": b"offsets", b"fvar": b"axes"}


def normalized_tables():
    tables = source_tables()
    head = bytearray(tables[b"head"])
    head[8:12] = b"sum!"
    head[16] = 8
    head[51] = 1
    tables[b"head"] = bytes(head)
    tables[b"glyf"] = b"normalized outlines"
    tables[b"loca"] = b"normalized offsets"
    return tables


def mock_encode(_encoder, source, encoded, decoded):
    """Supply deterministic tiny files to test generation without running a codec."""
    tables = sfnt_tables(Path(source).read_bytes())
    tables[b"head"] = normalized_tables()[b"head"]
    reconstructed = font(tables)
    header = bytearray(48)
    header[:4] = b"wOF2"
    header[4:8] = b"\x00\x01\x00\x00"
    struct.pack_into(">H", header, 12, 1)
    body = bytes([47, len(tables[b"fvar"])]) + tables[b"fvar"]
    struct.pack_into(">I", header, 8, len(header) + len(body))
    struct.pack_into(">I", header, 16, len(reconstructed))
    Path(encoded).write_bytes(header + body)
    Path(decoded).write_bytes(reconstructed)


def write_catalog_fixture(root):
    """Create a small, independently varied twelve-entry catalog fixture."""
    pins = {"commit": "a" * 40, "fonts": []}
    request = {"inputs": [], "licenses": [], "native_sources": []}
    for index in range(12):
        repo = f"gfont_test{index}"
        source_path = root / (repo + ".ttf")
        license_path = root / (repo + ".txt")
        tables = source_tables()
        tables[b"fvar"] = f"axis{index}".encode("ascii")
        data = font(tables)
        source_path.write_bytes(data)
        license_path.write_text("SIL OPEN FONT LICENSE fixture", encoding="utf-8")
        pins["fonts"].append({
            "family": f"Test {index}", "category": "SansSerif", "repo": repo,
            "file": source_path.name, "url": "https://example.test/fonts/" + source_path.name,
            "var": f"kGFTest{index}Woff2", "bytes": len(data), "sha256": sha256(data),
        })
        request["inputs"].append(str(source_path))
        request["licenses"].append(str(license_path))
        request["native_sources"].append(str(root / (repo + ".cc")))
    encoder = root / "encoder.cc"
    encoder.write_text("// source fixture\n", encoding="utf-8")
    request["codec_sources"] = [{"name": encoder.name, "path": str(encoder)}]
    (root / "request.json").write_text(json.dumps(request), encoding="utf-8")
    (root / "pins.json").write_text(json.dumps(pins), encoding="utf-8")
    (root / "deps.bzl").write_text(
        'new_git_repository(\n name = "woff2",\n commit = "' + "a" * 40 + '",\n)\n',
        encoding="utf-8",
    )
    (root / "MODULE.bazel").write_text(
        'bazel_dep(name = "brotli", version = "1.2.0")\n', encoding="utf-8",
    )
    return SimpleNamespace(
        request=root / "request.json", pins=root / "pins.json", encoder="unused",
        encoder_source=encoder, dependency_pins=root / "deps.bzl", module=root / "MODULE.bazel",
        assets=root / "assets", metadata=root / "catalog.inc", header=root / "fonts.h",
        manifest=root / "manifest.json", notices=root / "notices.txt",
    )


class CatalogGeneratorTest(unittest.TestCase):
    def test_complete_generation_keeps_payloads_separate_and_paths_content_addressed(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = write_catalog_fixture(Path(temporary))
            with mock.patch("catalog_generate.encode_font", side_effect=mock_encode) as encode:
                generate(args)
                self.assertEqual(encode.call_count, 12)
            manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
            self.assertEqual(len(manifest["fonts"]), 12)
            self.assertEqual(len(list((args.assets / "fonts").glob("*.woff2"))), 12)
            metadata = args.metadata.read_text(encoding="utf-8")
            self.assertNotIn("#include", metadata)
            self.assertNotIn("unsigned char", metadata)
            for record in manifest["fonts"]:
                path = args.assets / record["path"]
                self.assertEqual(path.stem, sha256(path.read_bytes()))
                self.assertIn(record["sha256"], metadata)
            self.assertEqual(args.notices.read_text(encoding="utf-8").count("LICENSE fixture"), 12)

    def test_accepts_only_documented_head_normalization(self):
        verify_preserved_tables(font(source_tables()), font(normalized_tables()))

    def test_rejects_axis_layout_name_and_metric_changes(self):
        for tag in (b"fvar", b"gvar", b"GSUB", b"GPOS", b"name", b"hmtx", b"cmap"):
            with self.subTest(table=tag):
                before = source_tables()
                after = normalized_tables()
                before[tag] = b"complete original data"
                after[tag] = b"modified data"
                with self.assertRaisesRegex(ValueError, "changed table"):
                    verify_preserved_tables(font(before), font(after))

    def test_rejects_removed_variation_table(self):
        after = normalized_tables()
        del after[b"fvar"]
        with self.assertRaisesRegex(ValueError, "table set"):
            verify_preserved_tables(font(source_tables()), font(after))

    def test_rejects_unexpected_head_flags_and_metrics(self):
        for offset in (16, 17, 18, 20, 36, 50):
            with self.subTest(offset=offset):
                after = normalized_tables()
                head = bytearray(after[b"head"])
                head[offset] ^= 1
                after[b"head"] = bytes(head)
                with self.assertRaises(ValueError):
                    verify_preserved_tables(font(source_tables()), font(after))

    def test_only_invalidated_digital_signature_may_be_removed(self):
        before = source_tables()
        before[b"DSIG"] = b"signature over original bytes"
        verify_preserved_tables(font(before), font(normalized_tables()))

    def test_rejects_duplicate_and_out_of_range_tables(self):
        duplicate = bytearray(font({b"cmap": b"one", b"name": b"two"}))
        duplicate[28:32] = b"cmap"
        oversized = bytearray(font({b"cmap": b"one"}))
        struct.pack_into(">I", oversized, 24, 0xFFFFFFFF)
        for data in (duplicate, oversized, b"OTTO", bytes(12)):
            with self.subTest(data=data):
                with self.assertRaises(ValueError):
                    sfnt_tables(data)

    def test_input_read_enforces_catalog_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "font.ttf"
            path.write_bytes(b"x")
            self.assertEqual(read_bounded(path), b"x")
            with path.open("wb") as stream:
                stream.truncate(MAX_FONT_BYTES + 1)
            with self.assertRaisesRegex(ValueError, "size outside"):
                read_bounded(path)

    def test_source_pin_mismatch_is_rejected_before_encoder(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "font.ttf"
            path.write_bytes(b"changed source")
            pin = {"bytes": 14, "sha256": "0" * 64, "family": "Changed font"}
            with mock.patch("catalog_generate.encode_font") as encode:
                with self.assertRaisesRegex(ValueError, "does not match pin"):
                    generate_font("unused", pin, path, "unused", temporary)
                encode.assert_not_called()

    def test_catalog_count_and_ordered_inputs_must_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pins = root / "pins.json"
            request = root / "request.json"
            args = SimpleNamespace(pins=pins, request=request)
            request.write_text("{}", encoding="utf-8")
            pins.write_text(json.dumps({"fonts": []}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly twelve"):
                read_catalog_request(args)
            pins.write_text(json.dumps({"fonts": [{}] * 12}), encoding="utf-8")
            request.write_text(json.dumps({"inputs": []}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "inputs do not match"):
                read_catalog_request(args)

    def test_encoded_header_lengths_are_exact(self):
        encoded = bytearray(48)
        encoded[:4] = b"wOF2"
        struct.pack_into(">I", encoded, 8, 48)
        struct.pack_into(">I", encoded, 16, 4)
        validate_encoded_font(encoded, b"sfnt")
        for offset, value in ((8, 47), (16, 3), (16, 5), (16, MAX_FONT_BYTES + 1)):
            with self.subTest(offset=offset, declared=value):
                invalid = bytearray(encoded)
                struct.pack_into(">I", invalid, offset, value)
                with self.assertRaises(ValueError):
                    validate_encoded_font(invalid, b"sfnt")

    def test_workspace_rejects_unsupported_flavors_tables_and_expansion(self):
        header = bytearray(48)
        header[4:8] = b"\x00\x01\x00\x00"
        struct.pack_into(">H", header, 12, 1)
        self.assertEqual(woff2_workspace(header + bytes([1, 54]))["intermediate_bytes"], 54)
        bad_flavor = bytearray(header)
        bad_flavor[4:8] = b"OTTO"
        too_many_tables = bytearray(header)
        struct.pack_into(">H", too_many_tables, 12, 65)
        for invalid in (
            bad_flavor, too_many_tables, header, header + bytes([13, 1]),
            header + bytes([63]) + b"CFF2" + bytes([1]),
            header + bytes([1, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F]),
            header + bytes([10, 1, 0xA0, 0x80, 1]),
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    woff2_workspace(invalid)

    def test_memory_gate_includes_two_loaders_and_one_old_cache_body(self):
        records = [
            {"encoded_bytes": 300000, "decoded_bytes": 900000},
            {"encoded_bytes": 200000, "decoded_bytes": 600000},
        ]
        budget = catalog_memory_budget(records)
        decoder_workspace = 2 * 1024 * 1024 + 29 * 512 * 1024 + 2 * 1024 * 1024
        self.assertEqual(
            budget["calculated_transient_bytes"],
            1200000 + decoder_workspace + 8 * 500000 + 2 * 1024 * 1024,
        )
        oversized = [{"encoded_bytes": MAX_FONT_BYTES, "decoded_bytes": MAX_FONT_BYTES}] * 2
        with self.assertRaisesRegex(ValueError, "transient loading bound"):
            catalog_memory_budget(oversized)

    def test_provenance_reads_authoritative_pins_and_rejects_ambiguity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            deps = root / "deps.bzl"
            module = root / "MODULE.bazel"
            source = root / "encoder.cc"
            block = 'new_git_repository(\n name = "woff2",\n commit = "' + "a" * 40 + '",\n)\n'
            deps.write_text(block, encoding="utf-8")
            module.write_text('bazel_dep(name = "brotli", version = "1.2.0")\n', encoding="utf-8")
            source.write_text("int main() {}\n", encoding="utf-8")
            sources = [{"name": "encoder.cc", "path": str(source)}]
            self.assertEqual(
                encoder_provenance(deps, module, source, sources)["woff2_commit"], "a" * 40,
            )
            deps.write_text(block + block, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "one pinned"):
                encoder_provenance(deps, module, source, sources)

    def test_codec_fingerprint_covers_patches_and_is_order_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "one.cc"
            second = root / "two.cc"
            first.write_bytes(b"before")
            second.write_bytes(b"source")
            sources = [{"name": path.name, "path": str(path)} for path in (first, second)]
            original = codec_fingerprint(sources)
            self.assertEqual(codec_fingerprint(list(reversed(sources))), original)
            first.write_bytes(b"patched")
            self.assertNotEqual(codec_fingerprint(sources)["sha256"], original["sha256"])
            with self.assertRaisesRegex(ValueError, "ambiguous"):
                codec_fingerprint(sources + sources)


if __name__ == "__main__":
    unittest.main()
