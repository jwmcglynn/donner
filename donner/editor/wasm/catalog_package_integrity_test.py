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


def _validate_resource_identity(asset: dict) -> None:
    extensions = {"font/woff2": "woff2", "font/ttf": "ttf", "font/otf": "otf",
                  "image/svg+xml": "svg"}
    digest = asset.get("sha256", "")
    media = asset.get("mediaType")
    if (not re.fullmatch(r"[a-f0-9]{64}", digest) or media not in extensions
            or asset.get("path") != f"assets/{digest}.{extensions[media]}"
            or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,127}", asset.get("id", ""))
            or asset.get("group") not in {"ui-font", "fallback-font", "chrome", "sample", "catalog-font"}
            or type(asset.get("encodedSize")) is not int
            or not 0 < asset["encodedSize"] <= 8 * 1024 * 1024
            or type(asset.get("decodedSize")) is not int
            or not 0 < asset["decodedSize"] <= 8 * 1024 * 1024):
        raise ValueError("Invalid packaged resource identity")


def _validate_resource_payload(package: Path, asset: dict, module: bytes) -> int:
    payload_path = package / asset["path"]
    if payload_path.is_symlink() or payload_path.stat().st_size != asset["encodedSize"]:
        raise ValueError("Invalid packaged resource file")
    payload = payload_path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != asset["sha256"]:
        raise ValueError("Packaged resource digest mismatch")
    if payload in module:
        raise ValueError(f"Resource payload was linked into Wasm: {asset['id']}")
    return len(payload)


def validate_resources(package: Path, module: bytes) -> dict:
    manifest_path = package / "packaged-resources.json"
    if manifest_path.stat().st_size > 128 * 1024:
        raise ValueError("Resource metadata exceeds its limit")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schemaVersion") != 1 or not isinstance(manifest.get("assets"), list):
        raise ValueError("Invalid packaged resource manifest")
    expected = set()
    identities = set()
    total = 0
    for asset in manifest["assets"]:
        _validate_resource_identity(asset)
        if asset["id"] in identities:
            raise ValueError("Duplicate packaged resource identity")
        payload_size = _validate_resource_payload(package, asset, module)
        expected.add(asset["path"])
        identities.add(asset["id"])
        total += payload_size
    actual = {path.relative_to(package).as_posix() for path in (package / "assets").rglob("*")
              if path.is_file()}
    if actual != expected or len(identities) > 128 or total > 16 * 1024 * 1024:
        raise ValueError("Packaged resource file set or budget mismatch")
    serialized = json.dumps(manifest, separators=(",", ":"), sort_keys=True)
    if (package / "packaged-resources.js").read_text() != (
            "globalThis.DonnerPackagedResources = " + serialized + ";\n"):
        raise ValueError("Resource bootstrap metadata differs from audit manifest")
    return {"packaged_resource_count": len(identities), "packaged_resource_bytes": total}


def _validate_browser_bridge_exports(package: Path) -> None:
    path = package / "editor.js"
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("Browser glue exceeds inspection bound")
    glue = path.read_text()
    for name in ("callMain", "HEAPU8", "_malloc", "_free",
                 "_DonnerPackagedResourceManifest", "_DonnerInstallPackagedResource",
                 "_DonnerFailPackagedResource"):
        quoted = re.escape(name)
        binding = r"\bModule(?:\." + quoted + r"|\[['\"]" + quoted + r"['\"]\])\s*="
        if re.search(binding, glue) is None:
            raise ValueError(f"Missing public resource bridge export: {name}")


def validate_package(package: Path, native_notices: Path | None = None) -> dict:
    manifest = json.loads((package / "catalog-fonts.json").read_text())
    fonts = manifest["fonts"]
    if len(fonts) != 12 or {font["family"] for font in fonts} != FAMILIES:
        raise ValueError("The complete twelve-family catalog must be packaged")
    modules = list(package.glob("*.wasm"))
    if len(modules) != 1:
        raise ValueError("Expected exactly one Wasm module")
    module = modules[0].read_bytes()
    _validate_browser_bridge_exports(package)
    resources = validate_resources(package, module)
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
    return {"families": len(fonts), "deferred_font_bytes": total, "notice_bytes": len(notices), **resources}


class CatalogPackageValidationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / "fonts").mkdir()
        (self.package / "assets").mkdir()
        resource = b"<svg id='packaged-icon'/>"
        resource_hash = hashlib.sha256(resource).hexdigest()
        resource_path = f"assets/{resource_hash}.svg"
        (self.package / resource_path).write_bytes(resource)
        self.resources = {"schemaVersion": 1, "assets": [{
            "id": "kIcon", "sha256": resource_hash, "encodedSize": len(resource),
            "decodedSize": len(resource), "mediaType": "image/svg+xml", "group": "chrome",
            "path": resource_path,
        }]}
        serialized = json.dumps(self.resources, separators=(",", ":"), sort_keys=True)
        (self.package / "packaged-resources.json").write_text(serialized + "\n")
        (self.package / "packaged-resources.js").write_text(
            "globalThis.DonnerPackagedResources = " + serialized + ";\n")
        (self.package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0")
        self.glue = "\n".join(f"Module.{name}={name};" for name in (
            "callMain", "HEAPU8", "_malloc", "_free", "_DonnerPackagedResourceManifest",
            "_DonnerInstallPackagedResource", "_DonnerFailPackagedResource"))
        (self.package / "editor.js").write_text(self.glue)
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

    def test_external_resource_payload_cannot_be_linked_into_module(self):
        payload = (self.package / self.resources["assets"][0]["path"]).read_bytes()
        (self.package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0" + payload)
        with self.assertRaisesRegex(ValueError, "Resource payload was linked"):
            validate_package(self.package)

    def test_external_resource_corruption_is_rejected(self):
        (self.package / self.resources["assets"][0]["path"]).write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "resource file"):
            validate_package(self.package)

    def test_bootstrap_resource_metadata_must_match(self):
        (self.package / "packaged-resources.js").write_text("globalThis.DonnerPackagedResources = {};")
        with self.assertRaisesRegex(ValueError, "bootstrap metadata differs"):
            validate_package(self.package)

    def test_resource_startup_requires_actual_public_module_bindings(self):
        for name in ("callMain", "HEAPU8", "_malloc", "_free",
                     "_DonnerPackagedResourceManifest", "_DonnerInstallPackagedResource",
                     "_DonnerFailPackagedResource"):
            with self.subTest(export=name):
                (self.package / "editor.js").write_text(
                    self.glue.replace(f"Module.{name}={name};", ""))
                with self.assertRaisesRegex(ValueError, "Missing public resource bridge export"):
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
