"""Verify deferred catalog assets and absence of their payloads from the shipped Wasm module."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
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
    if payload_path.is_symlink():
        raise ValueError(f"Invalid packaged resource file: symlink (id={asset['id']})")
    actual_size = payload_path.stat().st_size
    if actual_size != asset["encodedSize"]:
        raise ValueError(f"Invalid packaged resource file: size (id={asset['id']}, "
                         f"expected={asset['encodedSize']}, actual={actual_size})")
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


# These bound build-input materialization, independently of shipping resource budgets.
MAX_BUILD_INPUT_ENTRIES = 256
MAX_BUILD_INPUT_FILE_BYTES = 32 * 1024 * 1024
MAX_BUILD_INPUT_TOTAL_BYTES = 64 * 1024 * 1024


def _build_input_size(entry: os.DirEntry, path: Path, relative: Path,
                      canonical: Path | None) -> int | None:
    metadata = entry.stat(follow_symlinks=False)
    if stat.S_ISDIR(metadata.st_mode):
        return None
    if stat.S_ISLNK(metadata.st_mode) and canonical is not None:
        metadata = path.resolve(strict=True).stat()
    if not stat.S_ISREG(metadata.st_mode):
        raise ValueError("Declared Bazel package contains a directory link or special file")
    if canonical is not None and path.resolve(strict=True) != canonical / relative:
        raise ValueError("Bazel package leaf does not match its canonical tree")
    if not 0 <= metadata.st_size <= MAX_BUILD_INPUT_FILE_BYTES:
        raise ValueError("Declared Bazel package exceeds copy byte limit")
    return metadata.st_size


def _build_input_inventory(root: Path, canonical: Path | None = None) -> dict[Path, int | None]:
    inventory = {}
    pending = [root]
    total = 0
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                if len(inventory) >= MAX_BUILD_INPUT_ENTRIES:
                    raise ValueError("Declared Bazel package exceeds copy entry limit")
                path = Path(entry.path)
                relative = path.relative_to(root)
                size = _build_input_size(entry, path, relative, canonical)
                inventory[relative] = size
                if size is None:
                    pending.append(path)
                    continue
                total += size
                if total > MAX_BUILD_INPUT_TOTAL_BYTES:
                    raise ValueError("Declared Bazel package exceeds copy byte limit")
    return inventory


def _copy_build_input(source: Path, destination: Path, size: int) -> None:
    descriptor = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_size != size:
            raise ValueError("Declared Bazel package input changed before copying")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(size + 1)
        after = os.fstat(descriptor)
        if (len(payload) != size or (before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                != (after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
            raise ValueError("Declared Bazel package input changed while copying")
        with destination.open("xb") as output:
            output.write(payload)
    finally:
        os.close(descriptor)


def validate_bazel_package(package: Path, native_notices: Path | None = None) -> dict:
    """Copy immutable Bazel-declared inputs before strict package validation.

    The caller must preserve the canonical tree for the whole call. A declared
    package-root alias is allowed; interior directory symlinks are rejected.
    """
    if package.is_absolute() or not package.parts or ".." in package.parts:
        raise ValueError("Expected a relative Bazel runfiles package path")
    visible = Path.cwd() / package
    try:
        anchor = (visible / "packaged-resources.json").resolve(strict=True)
        canonical = anchor.parent
        if anchor.name != "packaged-resources.json":
            raise ValueError("Bazel package manifest does not identify its canonical tree")
        if canonical != visible:
            prefix = canonical.parts[:-len(package.parts)]
            if (canonical.parts[-len(package.parts):] != package.parts or len(prefix) < 3
                    or prefix[-3] != "bazel-out" or prefix[-1] != "bin"):
                raise ValueError(
                    "Bazel package manifest does not identify its declared output tree")
        inventory = _build_input_inventory(canonical)
        manifest_size = inventory.get(Path("packaged-resources.json"))
        if manifest_size is None or manifest_size > 128 * 1024:
            raise ValueError("Resource metadata exceeds its limit")
        if _build_input_inventory(visible, canonical) != inventory:
            raise ValueError("Declared Bazel package inventory differs from its canonical tree")
        with tempfile.TemporaryDirectory() as temporary:
            materialized = Path(temporary) / "package"
            materialized.mkdir()
            for relative, size in sorted(inventory.items()):
                destination = materialized / relative
                if size is None:
                    destination.mkdir(parents=True, exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    _copy_build_input(canonical / relative, destination, size)
            return validate_package(materialized, native_notices)
    except OSError as error:
        raise ValueError(f"Invalid declared Bazel package input (errno={error.errno})") from None
    except RuntimeError:
        raise ValueError("Invalid declared Bazel package link resolution") from None


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

    def _package_cli(self, package, *, cwd=None, bazel_inputs=False, native_notices=None):
        if native_notices is None:
            native_notices = self.package / "CatalogFontNotices.txt"
        command = [sys.executable, str(Path(__file__).resolve()), "--package", str(package),
                   "--native-notices", str(native_notices)]
        if bazel_inputs:
            command.append("--bazel-inputs")
        return subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=10,
                              check=False)

    def _tree_snapshot(self, root):
        snapshot = {}
        for path in sorted(root.rglob("*")):
            relative = path.relative_to(root).as_posix()
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                snapshot[relative] = ("link", str(path.readlink()))
            elif stat.S_ISREG(metadata.st_mode):
                snapshot[relative] = ("file", stat.S_IMODE(metadata.st_mode), path.read_bytes())
            else:
                self.assertTrue(stat.S_ISDIR(metadata.st_mode), relative)
                snapshot[relative] = ("directory", stat.S_IMODE(metadata.st_mode))
        return snapshot

    def _declared_input_fixture(self, *, virtual=True, mutate=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        relative = Path("donner/editor/wasm/wasm_web_package")
        canonical = root / "execroot/bazel-out/wasm-transition/bin" / relative
        shutil.copytree(self.package, canonical)
        if mutate is not None:
            mutate(canonical)
        for path in canonical.rglob("*"):
            if path.is_file():
                path.chmod(0o444)
        cwd = root / "sandbox/catalog_test.runfiles/_main"
        visible = cwd / relative
        if virtual:
            visible.mkdir(parents=True)
            for path in sorted(canonical.rglob("*")):
                destination = visible / path.relative_to(canonical)
                if path.is_dir():
                    destination.mkdir()
                else:
                    destination.symlink_to(path)
        else:
            shutil.copytree(canonical, visible)
        notices = cwd / "third_party/google_fonts/CatalogFontNotices.txt"
        notices.parent.mkdir(parents=True)
        notices.symlink_to(self.package / "CatalogFontNotices.txt")
        return cwd, relative, canonical

    def _declared_input_cli(self, cwd, relative, canonical):
        before = self._tree_snapshot(canonical)
        source_before = self._tree_snapshot(self.package)
        transport_before = self._tree_snapshot(cwd)
        result = self._package_cli(
            relative, cwd=cwd, bazel_inputs=True,
            native_notices=Path("third_party/google_fonts/CatalogFontNotices.txt"))
        self.assertEqual(self._tree_snapshot(canonical), before)
        self.assertEqual(self._tree_snapshot(self.package), source_before)
        self.assertEqual(self._tree_snapshot(cwd), transport_before)
        return result

    def test_physical_package_cli_passes_without_changing_input(self):
        before = self._tree_snapshot(self.package)
        result = self._package_cli(self.package)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), validate_package(self.package))
        self.assertEqual(self._tree_snapshot(self.package), before)

    def test_bazel_input_cli_accepts_physical_and_complete_virtual_trees(self):
        for virtual in (False, True):
            with self.subTest(virtual=virtual):
                cwd, relative, canonical = self._declared_input_fixture(virtual=virtual)
                result = self._declared_input_cli(cwd, relative, canonical)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), validate_package(self.package))

    def test_bazel_input_cli_accepts_declared_package_root_alias(self):
        cwd, relative, canonical = self._declared_input_fixture()
        visible = cwd / relative
        shutil.rmtree(visible)
        visible.symlink_to(canonical, target_is_directory=True)
        self.assertTrue(visible.is_symlink())
        result = self._declared_input_cli(cwd, relative, canonical)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), validate_package(self.package))
        self.assertEqual(visible.readlink(), canonical)

    def test_default_validation_still_rejects_resource_leaf_links(self):
        cwd, relative, _ = self._declared_input_fixture()
        with self.assertRaisesRegex(ValueError, "Invalid packaged resource file"):
            validate_package(cwd / relative)
        result = self._package_cli(relative, cwd=cwd)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Invalid packaged resource file", result.stderr)

    def test_bazel_input_cli_rejects_byte_identical_wrong_tree_links(self):
        for selected in (self.resources["assets"][0]["path"], "packaged-resources.json"):
            with self.subTest(selected=selected):
                cwd, relative, canonical = self._declared_input_fixture()
                other = cwd.parent / "other-output" / relative
                shutil.copytree(canonical, other)
                leaf = cwd / relative / selected
                leaf.unlink()
                leaf.symlink_to(other / selected)
                other_before = self._tree_snapshot(other)
                result = self._declared_input_cli(cwd, relative, canonical)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self._tree_snapshot(other), other_before)

    def test_bazel_input_cli_rejects_dangling_payload_links(self):
        cwd, relative, canonical = self._declared_input_fixture()
        leaf = cwd / relative / self.resources["assets"][0]["path"]
        leaf.unlink()
        leaf.symlink_to(canonical / "missing.svg")
        result = self._declared_input_cli(cwd, relative, canonical)
        self.assertNotEqual(result.returncode, 0)

    def test_bazel_input_cli_rejects_linked_directories(self):
        for directory in ("assets", "fonts"):
            with self.subTest(directory=directory):
                cwd, relative, canonical = self._declared_input_fixture()
                selected = cwd / relative / directory
                shutil.rmtree(selected)
                selected.symlink_to(canonical / directory, target_is_directory=True)
                result = self._declared_input_cli(cwd, relative, canonical)
                self.assertNotEqual(result.returncode, 0)

    def test_bazel_input_materialization_preserves_integrity_failures(self):
        resource_path = self.resources["assets"][0]["path"]
        resource = (self.package / resource_path).read_bytes()
        cases = {
            "size": lambda package: (package / resource_path).write_bytes(b"wrong"),
            "digest": lambda package: (package / resource_path).write_bytes(
                resource[:-1] + bytes([resource[-1] ^ 1])),
            "missing": lambda package: (package / resource_path).unlink(),
            "extra": lambda package: (package / "assets/extra.svg").write_bytes(b"extra"),
            "embedded": lambda package: (package / "editor.wasm").write_bytes(
                b"\0asm\1\0\0\0" + resource),
            "bootstrap": lambda package: (package / "packaged-resources.js").write_text(
                "globalThis.DonnerPackagedResources = {};"),
            "notices": lambda package: (package / "CatalogFontNotices.txt").write_text(
                "Modified SIL OPEN FONT LICENSE Version 1.1"),
        }
        for name, mutate in cases.items():
            with self.subTest(mutation=name):
                cwd, relative, canonical = self._declared_input_fixture(mutate=mutate)
                strict = self._package_cli(canonical)
                result = self._declared_input_cli(cwd, relative, canonical)
                self.assertNotEqual(strict.returncode, 0)
                self.assertNotEqual(result.returncode, 0)
                if name == "digest":
                    self.assertIn("Packaged resource digest mismatch", strict.stderr)

    def test_complete_deferred_package_passes(self):
        self.assertEqual(validate_package(self.package)["deferred_font_bytes"], 64 * 12)

    def test_compact_shipping_catalog_preserves_audit_and_payload_authentication(self):
        from donner.editor.wasm.package_web_files import package_files

        readable = json.dumps(self.manifest, indent=2).encode() + b"\n"
        (self.package / "catalog-fonts.json").write_bytes(readable)
        shipping = self.package / "shipping"
        files = [path for path in self.package.iterdir() if path.is_file()]
        assets = [(path.relative_to(self.package).as_posix(), path)
                  for path in self.package.rglob("*")
                  if path.is_file() and path.parent != self.package]
        package_files(shipping, files, assets)
        self.assertEqual(json.loads((shipping / "catalog-fonts.json").read_text()), self.manifest)
        self.assertLess((shipping / "catalog-fonts.json").stat().st_size, len(readable))
        self.assertEqual((self.package / "catalog-fonts.json").read_bytes(), readable)
        self.assertEqual(validate_package(shipping), validate_package(self.package))
        for path in [*files, *(source for _, source in assets)]:
            if path.name != "catalog-fonts.json":
                self.assertEqual((shipping / path.relative_to(self.package)).read_bytes(),
                                 path.read_bytes())
        font = self.manifest["fonts"][0]
        payload = shipping / font["path"]
        original = payload.read_bytes()
        payload.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, "integrity failed"):
            validate_package(shipping)
        payload.write_bytes(original)
        altered = json.loads((shipping / "catalog-fonts.json").read_text())
        altered["fonts"][0]["path"] = "fonts/untrusted.woff2"
        (shipping / "catalog-fonts.json").write_text(json.dumps(altered))
        with self.assertRaisesRegex(ValueError, "Noncanonical"):
            validate_package(shipping)

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
    parser.add_argument("--bazel-inputs", action="store_true",
                        help="Materialize declared Bazel package inputs before strict validation")
    options, remaining = parser.parse_known_args()
    if options.package:
        validator = validate_bazel_package if options.bazel_inputs else validate_package
        print(json.dumps(validator(options.package, options.native_notices), sort_keys=True))
    elif options.bazel_inputs:
        parser.error("--bazel-inputs requires --package")
    else:
        unittest.main(argv=[__file__, *remaining])
