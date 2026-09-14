#!/usr/bin/env python3
"""Build deterministic, content-addressed catalog fonts from pinned sfnt files."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MAX_FONT_BYTES = 2 * 1024 * 1024
MAX_CATALOG_BYTES = 4 * 1024 * 1024
MAX_INTERMEDIATE_BYTES = 2 * 1024 * 1024
MAX_TRANSFORMED_GLYF_BYTES = 512 * 1024
MAX_TABLE_COUNT = 64
MAX_BROTLI_BYTES = 8 * 1024 * 1024
MAX_TRANSIENT_BYTES = 32 * 1024 * 1024
GLYPH_SCRATCH_BYTES_PER_TRANSFORMED_BYTE = 29
GLYPH_TABLE_SCRATCH_ALLOWANCE = 2 * 1024 * 1024
SFNT_VALIDATION_WORKSPACE = 4 * 1024 * 1024
BROKER_ENCODED_COPIES_PER_LOADER = 8
PERSISTENT_CACHE_BODY_ALLOWANCE = 2 * 1024 * 1024
EXPECTED_FONT_COUNT = 12


def sha256(data):
    """Return the lowercase content address of bytes."""
    return hashlib.sha256(data).hexdigest()


def read_bounded(path):
    """Read a font without allowing a changed input to allocate beyond its cap."""
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_FONT_BYTES + 1)
    if not data or len(data) > MAX_FONT_BYTES:
        raise ValueError(f"Font size outside catalog limit: {path}")
    return data


def sfnt_tables(data):
    """Read the bounded table directory needed for semantic roundtrip checks."""
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO"):
        raise ValueError("Expected a single sfnt font")
    count = struct.unpack_from(">H", data, 4)[0]
    if count == 0 or count > 256 or 12 + count * 16 > len(data):
        raise ValueError("Invalid sfnt table count")
    tables = {}
    for index in range(count):
        tag, _, offset, size = struct.unpack_from(">4sIII", data, 12 + index * 16)
        if tag in tables or offset > len(data) or size > len(data) - offset:
            raise ValueError("Invalid or duplicate sfnt table")
        tables[tag] = data[offset:offset + size]
    return tables


def verify_preserved_tables(source, decoded):
    """Require byte identity for all tables outside WOFF2 outline normalization."""
    before = sfnt_tables(source)
    after = sfnt_tables(decoded)
    # A digital signature over the original sfnt is invalid after reconstruction.
    if before.keys() - {b"DSIG"} != after.keys():
        raise ValueError("WOFF2 roundtrip changed the font's table set")
    for tag, data in before.items():
        if tag not in (b"DSIG", b"head", b"glyf", b"loca") and after[tag] != data:
            raise ValueError(f"WOFF2 roundtrip changed table {tag!r}")
    before_head = bytearray(before[b"head"])
    after_head = bytearray(after[b"head"])
    if len(before_head) != 54 or len(after_head) != 54:
        raise ValueError("Invalid head table length")
    expected_flags = struct.unpack_from(">H", before_head, 16)[0] | 0x0800
    if struct.unpack_from(">H", after_head, 16)[0] != expected_flags:
        raise ValueError("WOFF2 roundtrip changed head flags beyond the lossless-transform bit")
    source_loca = struct.unpack_from(">h", before_head, 50)[0]
    decoded_loca = struct.unpack_from(">h", after_head, 50)[0]
    if (source_loca, decoded_loca) not in ((0, 0), (0, 1), (1, 1)):
        raise ValueError("WOFF2 roundtrip changed the outline index format unexpectedly")
    for start, end in ((8, 12), (16, 18), (50, 52)):
        before_head[start:end] = after_head[start:end]
    if before_head != after_head:
        raise ValueError("WOFF2 roundtrip changed head metrics or font revision")


def codec_fingerprint(sources):
    """Fingerprint configured source bytes, including any decoder source patches."""
    if not sources or len({source["name"] for source in sources}) != len(sources):
        raise ValueError("Missing or ambiguous WOFF2 source inventory")
    files = [
        {"name": source["name"], "sha256": sha256(Path(source["path"]).read_bytes())}
        for source in sources
    ]
    files.sort(key=lambda source: source["name"])
    return {
        "sha256": sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode("utf-8")),
        "files": files,
    }


def encoder_provenance(dependency_pins, module, encoder_source, codec_sources):
    """Extract versions from authoritative build pins, rejecting ambiguous edits."""
    woff2 = re.findall(
        r'new_git_repository\(\s*name\s*=\s*"woff2",(.*?)\n\s*\)',
        Path(dependency_pins).read_text(encoding="utf-8"), re.DOTALL,
    )
    if len(woff2) != 1:
        raise ValueError("Expected one pinned WOFF2 repository")
    commits = re.findall(r'\bcommit\s*=\s*"([a-f0-9]{40})"', woff2[0])
    brotli = re.findall(
        r'bazel_dep\(name\s*=\s*"brotli",\s*version\s*=\s*"([^"]+)"\)',
        Path(module).read_text(encoding="utf-8"),
    )
    if len(commits) != 1 or len(brotli) != 1:
        raise ValueError("Missing or ambiguous encoder dependency pin")
    return {
        "woff2_commit": commits[0],
        "brotli_module_version": brotli[0],
        "wrapper_sha256": sha256(Path(encoder_source).read_bytes()),
        "generator_sha256": sha256(Path(__file__).read_bytes()),
        "configured_woff2_sources": codec_fingerprint(codec_sources),
        "brotli_quality": 11,
        "allow_transforms": True,
        "extended_metadata": "",
        "roundtrip_brotli_memory_limit": MAX_BROTLI_BYTES,
    }


def encode_font(encoder, source, encoded, decoded):
    """Invoke the pinned host tool without relying on shell or ambient encoders."""
    subprocess.run(
        [str(Path(encoder).resolve()), str(source), str(encoded), str(decoded)],
        check=True,
        timeout=120,
    )


def write_text(path, text):
    """Write a generated UTF-8 file using deterministic line endings."""
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8", newline="\n")


def native_source(symbol, data):
    """Generate immutable arrays; metadata-only consumers never link this file."""
    lines = [
        "// Generated catalog font bytes. Do not edit.",
        '#include "third_party/google_fonts/embed_resources/GoogleFontsData.h"',
        "",
        "namespace donner::embedded {",
        f"const unsigned char {symbol}Data[{len(data)}] = {{",
    ]
    for offset in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    lines.extend(["};", "}  // namespace donner::embedded", ""])
    return "\n".join(lines)


def read_catalog_request(args):
    """Validate the fixed catalog and ordered action inputs before invoking tools."""
    request = json.loads(Path(args.request).read_text(encoding="utf-8"))
    pins = json.loads(Path(args.pins).read_text(encoding="utf-8"))
    fonts = pins["fonts"]
    if len(fonts) != EXPECTED_FONT_COUNT:
        raise ValueError("The curated catalog must contain exactly twelve fonts")
    for key in ("inputs", "licenses", "native_sources"):
        if len(request[key]) != len(fonts):
            raise ValueError(f"Catalog {key} do not match the font pins")
    for key in ("family", "repo", "var", "sha256"):
        if len({font[key] for font in fonts}) != len(fonts):
            raise ValueError(f"Duplicate catalog {key}")
    return request, pins


def validate_encoded_font(encoded, decoded):
    """Require exact encoded and reconstructed lengths for compiled metadata."""
    if encoded[:4] != b"wOF2" or len(encoded) < 48:
        raise ValueError("Encoder did not produce WOFF2")
    if struct.unpack_from(">I", encoded, 8)[0] != len(encoded):
        raise ValueError("Invalid encoded WOFF2 length")
    declared_decoded = struct.unpack_from(">I", encoded, 16)[0]
    if declared_decoded > MAX_FONT_BYTES or len(decoded) != declared_decoded:
        raise ValueError("Invalid expanded WOFF2 length")


class Woff2Directory:
    """Bounded cursor for the generated font's transform-length inventory."""

    def __init__(self, data):
        self.data = data
        self.offset = 48

    def read(self, size):
        if size > len(self.data) - self.offset:
            raise ValueError("Truncated WOFF2 table directory")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def base128(self):
        value = 0
        for index in range(5):
            byte = self.read(1)[0]
            if (index == 0 and byte == 0x80) or value & 0xFE000000:
                raise ValueError("Invalid WOFF2 base128 length")
            value = (value << 7) | (byte & 0x7F)
            if byte & 0x80 == 0:
                return value
        raise ValueError("Overlong WOFF2 base128 length")

    def table(self):
        flags = self.read(1)[0]
        index = flags & 0x3F
        tag = self.read(4) if index == 0x3F else {10: b"glyf", 11: b"loca", 13: b"CFF "}.get(index)
        if tag in (b"CFF ", b"CFF2"):
            raise ValueError("Catalog decoder workspace assumes TrueType outlines")
        original_length = self.base128()
        version = flags >> 6
        transformed = version == 0 if tag in (b"glyf", b"loca") else version != 0
        length = self.base128() if transformed else original_length
        if tag == b"loca" and transformed and length != 0:
            raise ValueError("Invalid transformed loca length")
        return tag, length, transformed


def woff2_workspace(encoded):
    """Enforce the same standalone TrueType workspace assumptions as the runtime."""
    if len(encoded) < 48 or encoded[4:8] != b"\x00\x01\x00\x00":
        raise ValueError("Catalog requires a standalone TrueType WOFF2 font")
    table_count = struct.unpack_from(">H", encoded, 12)[0]
    if not 0 < table_count <= MAX_TABLE_COUNT:
        raise ValueError("Catalog WOFF2 table count exceeds the bound")
    cursor = Woff2Directory(encoded)
    intermediate = 0
    transformed_glyf = 0
    for _ in range(table_count):
        tag, length, transformed = cursor.table()
        intermediate += length
        if tag == b"glyf" and transformed:
            transformed_glyf += length
    if intermediate > MAX_INTERMEDIATE_BYTES or transformed_glyf > MAX_TRANSFORMED_GLYF_BYTES:
        raise ValueError("Catalog WOFF2 intermediate/glyph workspace exceeds the bound")
    return {
        "table_count": table_count,
        "intermediate_bytes": intermediate,
        "transformed_glyf_bytes": transformed_glyf,
    }


def font_license(font, path):
    """Read the original OFL notice and produce its immutable provenance."""
    path = Path(path)
    if path.name != font["repo"] + ".txt":
        raise ValueError("License input order does not match font pins")
    with path.open("rb") as stream:
        data = stream.read(65537)
    if len(data) > 65536 or b"SIL OPEN FONT LICENSE" not in data:
        raise ValueError("Missing full OFL license text")
    url = font["url"].rsplit("/", 1)[0] + "/OFL.txt"
    text = "\n".join([font["family"], font["url"], url, "", data.decode("utf-8"), ""])
    return {"id": "OFL-1.1", "url": url, "sha256": sha256(data)}, text


def generate_font(encoder, font, source_path, license_path, temporary):
    """Encode one verified source and describe the resulting complete font."""
    source = read_bounded(source_path)
    if len(source) != font["bytes"] or sha256(source) != font["sha256"]:
        raise ValueError(f"Input does not match pin for {font['family']}")
    if not re.fullmatch(r"kGF[A-Za-z0-9]+Woff2", font["var"]):
        raise ValueError("Invalid native font symbol")
    if not re.fullmatch(r"[A-Za-z]+", font["category"]):
        raise ValueError("Invalid font category token")
    encoded_path = Path(temporary) / "font.woff2"
    decoded_path = Path(temporary) / "decoded.ttf"
    encode_font(encoder, source_path, encoded_path, decoded_path)
    encoded = read_bounded(encoded_path)
    decoded = read_bounded(decoded_path)
    verify_preserved_tables(source, decoded)
    validate_encoded_font(encoded, decoded)
    digest = sha256(encoded)
    license_record, notice = font_license(font, license_path)
    record = {
        "family": font["family"], "category": font["category"],
        "format": "woff2", "sha256": digest, "path": f"fonts/{digest}.woff2",
        "encoded_bytes": len(encoded), "decoded_bytes": len(decoded),
        "decoded_size_limit": len(decoded), "decoded_sha256": sha256(decoded),
        "native_symbol": font["var"],
        "decoder_workspace": woff2_workspace(encoded),
        "source": {key: font[key] for key in ("repo", "file", "url", "sha256", "bytes")},
        "license": license_record,
    }
    return record, encoded, notice


def metadata_entry(record):
    """Expose no payload symbol reference unless the consumer expands its token."""
    return "DONNER_GF_ENTRY(" + ", ".join([
        json.dumps(record["family"]), record["category"], json.dumps(record["sha256"]),
        json.dumps(record["path"]), str(record["encoded_bytes"]), str(record["decoded_bytes"]),
        record["native_symbol"],
    ]) + ")"


def catalog_memory_budget(fonts):
    """Bound controlled loading allocations; retained fonts and browser internals are separate."""
    if len(fonts) < 2:
        raise ValueError("Memory accounting requires at least two catalog assets")
    largest_encoded = sorted((font["encoded_bytes"] for font in fonts), reverse=True)[:2]
    glyph_workspace = (
        GLYPH_SCRATCH_BYTES_PER_TRANSFORMED_BYTE * MAX_TRANSFORMED_GLYF_BYTES
        + GLYPH_TABLE_SCRATCH_ALLOWANCE
    )
    decoder_workspace = max(
        MAX_INTERMEDIATE_BYTES + max(MAX_BROTLI_BYTES, glyph_workspace), SFNT_VALIDATION_WORKSPACE,
    )
    largest_face = max(font["encoded_bytes"] + font["decoded_bytes"] for font in fonts)
    decode_peak = largest_face + decoder_workspace
    broker_peak = BROKER_ENCODED_COPIES_PER_LOADER * sum(largest_encoded)
    total = decode_peak + broker_peak + PERSISTENT_CACHE_BODY_ALLOWANCE
    if total > MAX_TRANSIENT_BYTES:
        raise ValueError(f"Catalog transient loading bound {total} exceeds {MAX_TRANSIENT_BYTES}")
    return {
        "scope": "controlled_product_loading_allocations",
        "maximum_transient_bytes": MAX_TRANSIENT_BYTES,
        "calculated_transient_bytes": total,
        "largest_face_decode_peak_bytes": decode_peak,
        "two_loader_broker_peak_bytes": broker_peak,
        "largest_two_encoded_bytes": largest_encoded,
        "persistent_cache_body_allowance": PERSISTENT_CACHE_BODY_ALLOWANCE,
        "maximum_persistent_writes_in_flight": 1,
        "maximum_network_loaders": 2,
        "maximum_concurrent_decodes": 1,
        "broker_encoded_copies_per_loader": BROKER_ENCODED_COPIES_PER_LOADER,
        "maximum_brotli_bytes": MAX_BROTLI_BYTES,
        "maximum_intermediate_bytes": MAX_INTERMEDIATE_BYTES,
        "maximum_transformed_glyf_bytes": MAX_TRANSFORMED_GLYF_BYTES,
        "maximum_table_count": MAX_TABLE_COUNT,
        "glyph_scratch_bytes_per_transformed_byte": GLYPH_SCRATCH_BYTES_PER_TRANSFORMED_BYTE,
        "glyph_table_scratch_allowance": GLYPH_TABLE_SCRATCH_ALLOWANCE,
        "sfnt_validation_workspace": SFNT_VALIDATION_WORKSPACE,
        "separate_encoded_retention_limit": MAX_CATALOG_BYTES,
    }


def generate(args):
    """Generate metadata, native sources, notices and the complete web asset tree."""
    request, pins = read_catalog_request(args)

    assets = Path(args.assets)
    (assets / "fonts").mkdir(parents=True, exist_ok=True)
    metadata = ["// Generated catalog metadata. Do not edit."]
    header = [
        "#pragma once", "/// @file", "", "#include <span>", "",
        "namespace donner::embedded {",
    ]
    manifest = {
        "schema_version": 1,
        "source_commit": pins["commit"],
        "encoder": encoder_provenance(
            args.dependency_pins, args.module, args.encoder_source, request["codec_sources"],
        ),
        "fonts": [],
    }
    notices = ["Donner bundled font notices", ""]
    total_encoded = 0
    with tempfile.TemporaryDirectory(prefix="catalog-", dir=assets) as temporary:
        for index, font in enumerate(pins["fonts"]):
            record, encoded, notice = generate_font(
                args.encoder, font, request["inputs"][index], request["licenses"][index], temporary,
            )
            destination = assets / record["path"]
            if destination.exists():
                raise ValueError("Duplicate encoded catalog content")
            destination.write_bytes(encoded)
            total_encoded += len(encoded)
            if total_encoded > MAX_CATALOG_BYTES:
                raise ValueError("Encoded catalog exceeds the retention budget")
            notices.append(notice)
            manifest["fonts"].append(record)
            metadata.append(metadata_entry(record))
            symbol = font["var"]
            header.extend([
                f"extern const unsigned char {symbol}Data[{len(encoded)}];",
                "/// Encoded WOFF2 bytes in immutable program storage.",
                f"inline constexpr std::span<const unsigned char> {symbol}({symbol}Data);",
            ])
            write_text(request["native_sources"][index], native_source(symbol, encoded))
    header.extend(["}  // namespace donner::embedded", ""])
    manifest["encoded_bytes"] = total_encoded
    manifest["memory_budget"] = catalog_memory_budget(manifest["fonts"])
    manifest_text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    notice_text = "\n".join(notices)
    write_text(args.metadata, "\n".join(metadata) + "\n")
    write_text(args.header, "\n".join(header))
    write_text(args.manifest, manifest_text)
    write_text(args.notices, notice_text)
    write_text(assets / "catalog-fonts.json", manifest_text)
    write_text(assets / "CatalogFontNotices.txt", notice_text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in (
        "request", "pins", "encoder", "encoder-source", "dependency-pins", "module",
        "assets", "metadata", "header", "manifest", "notices",
    ):
        parser.add_argument("--" + name, required=True)
    generate(parser.parse_args())


if __name__ == "__main__":
    main()
