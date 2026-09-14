"""Verify deferred catalog assets and absence of their payloads from the shipped Wasm module."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import tempfile
import unittest


FAMILIES = {
    "Bebas Neue", "Bitter", "Inter", "JetBrains Mono", "Lato", "Lora", "Montserrat",
    "Open Sans", "Oswald", "Pacifico", "Playfair Display", "Roboto Mono",
}


def _validate_font(package: Path, font: dict, module: bytes) -> tuple[str, int]:
    digest = font["sha256"]
    if not re.fullmatch(r"[a-f0-9]{64}", digest):
        raise ValueError("Invalid catalog content identity")
    path = f"fonts/{digest}.woff2"
    if font["path"] != path:
        raise ValueError("Noncanonical or duplicate catalog asset path")
    payload = (package / path).read_bytes()
    if len(payload) != font["encoded_bytes"] or hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError(f"Catalog asset integrity failed: {font['family']}")
    if payload[:4] != b"wOF2" or struct.unpack_from(">I", payload, 16)[0] != font["decoded_bytes"]:
        raise ValueError(f"Catalog asset header differs: {font['family']}")
    if payload in module:
        raise ValueError(f"Catalog payload was linked into Wasm: {font['family']}")
    return path, len(payload)


def validate_package(package: Path, native_notices: Path | None = None) -> dict:
    manifest = json.loads((package / "catalog-fonts.json").read_text())
    fonts = manifest["fonts"]
    if len(fonts) != 12 or {font["family"] for font in fonts} != FAMILIES:
        raise ValueError("The complete twelve-family catalog must be packaged")
    modules = list(package.glob("*.wasm"))
    if len(modules) != 1:
        raise ValueError("Expected exactly one Wasm module")
    module = modules[0].read_bytes()
    expected_paths = set()
    total = 0
    for font in fonts:
        path, payload_bytes = _validate_font(package, font, module)
        if path in expected_paths:
            raise ValueError("Noncanonical or duplicate catalog asset path")
        expected_paths.add(path)
        total += payload_bytes
    actual_paths = {path.relative_to(package).as_posix() for path in (package / "fonts").rglob("*")
                    if path.is_file()}
    if actual_paths != expected_paths or total != manifest["encoded_bytes"]:
        raise ValueError("Catalog package file set or total bytes differ from its manifest")
    notices = (package / "CatalogFontNotices.txt").read_bytes()
    if native_notices is not None and notices != native_notices.read_bytes():
        raise ValueError("Native and deferred catalog notices differ")
    if b"SIL OPEN FONT LICENSE" not in notices:
        raise ValueError("Catalog font license text is missing")
    return {"families": len(fonts), "deferred_font_bytes": total, "notice_bytes": len(notices)}


class CatalogPackageValidationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / "fonts").mkdir()
        (self.package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0")
        fonts = []
        for index, family in enumerate(sorted(FAMILIES)):
            payload = bytearray([index] * 64)
            payload[:4] = b"wOF2"
            struct.pack_into(">I", payload, 16, 128)
            digest = hashlib.sha256(payload).hexdigest()
            path = f"fonts/{digest}.woff2"
            (self.package / path).write_bytes(payload)
            fonts.append({"family": family, "sha256": digest, "path": path,
                          "encoded_bytes": len(payload), "decoded_bytes": 128})
        self.manifest = {"fonts": fonts, "encoded_bytes": 64 * 12}
        (self.package / "catalog-fonts.json").write_text(json.dumps(self.manifest))
        (self.package / "CatalogFontNotices.txt").write_text("SIL OPEN FONT LICENSE Version 1.1")

    def test_complete_deferred_package_passes(self):
        self.assertEqual(validate_package(self.package)["deferred_font_bytes"], 64 * 12)

    def test_restored_payload_dependency_is_detected_in_actual_module(self):
        payload = (self.package / self.manifest["fonts"][0]["path"]).read_bytes()
        (self.package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0" + payload)
        with self.assertRaisesRegex(ValueError, "payload was linked into Wasm"):
            validate_package(self.package)

    def test_asset_corruption_is_rejected(self):
        (self.package / self.manifest["fonts"][0]["path"]).write_bytes(b"wrong font")
        with self.assertRaisesRegex(ValueError, "integrity failed"):
            validate_package(self.package)

    def test_unmanifested_payload_is_rejected(self):
        (self.package / "fonts/extra.woff2").write_bytes(b"unexpected")
        with self.assertRaisesRegex(ValueError, "file set or total"):
            validate_package(self.package)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--native-notices", type=Path)
    options, remaining = parser.parse_known_args()
    if options.package:
        print(json.dumps(validate_package(options.package, options.native_notices), sort_keys=True))
    else:
        unittest.main(argv=[__file__, *remaining])
