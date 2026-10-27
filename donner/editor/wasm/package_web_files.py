"""Copy explicitly declared application files and assets into a collision-checked web bundle."""

import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
import shutil


def _add_input(inputs: dict[str, Path], relative: str, source: Path) -> None:
    path = PurePosixPath(relative)
    if (
        not path.parts
        or path.is_absolute()
        or ".." in path.parts
        or path.as_posix() != relative
        or "\\" in relative
        or any(ord(character) < 32 for character in relative)
    ):
        raise ValueError(f"Invalid web package path: {relative!r}")
    if relative in inputs:
        raise ValueError(f"Duplicate web package path: {relative}")
    if not source.is_file():
        raise ValueError(f"Web package input is not a file: {source}")
    inputs[relative] = source


def _compact_catalog_manifest(inputs: dict[str, Path]) -> bytes | None:
    """Serialize the shipping audit copy without changing its parsed fields."""
    source = inputs.get("catalog-fonts.json")
    if source is None:
        return None
    with source.open("rb") as stream:
        readable = stream.read(128 * 1024 + 1)
    if len(readable) > 128 * 1024:
        raise ValueError("Catalog manifest exceeds metadata limit")
    manifest = json.loads(readable)
    return (json.dumps(manifest, separators=(",", ":"), sort_keys=True, allow_nan=False)
            + "\n").encode("utf-8")


def _copy_inputs(output: Path, inputs: dict[str, Path], catalog: bytes | None) -> None:
    if output.is_symlink():
        raise ValueError("Web package output must not be a symlink")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Web package output must be empty")
    for relative, source in sorted(inputs.items()):
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() or destination.is_symlink():
            raise ValueError(f"Web package output already exists: {relative}")
        if relative == "catalog-fonts.json" and catalog is not None:
            destination.write_bytes(catalog)
        else:
            shutil.copyfile(source, destination)


def _validate_resource_descriptor(asset: dict) -> None:
    extensions = {"font/woff2": "woff2", "font/ttf": "ttf", "font/otf": "otf",
                  "image/svg+xml": "svg"}
    digest = asset.get("sha256", "")
    media_type = asset.get("mediaType")
    if (not re.fullmatch(r"[a-f0-9]{64}", digest)
            or media_type not in extensions
            or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,127}", asset.get("id", ""))
            or asset.get("group") not in {"ui-font", "fallback-font", "chrome", "sample", "catalog-font"}
            or asset.get("path") != f"assets/{digest}.{extensions[media_type]}"
            or type(asset.get("encodedSize")) is not int
            or not 0 < asset["encodedSize"] <= 8 * 1024 * 1024
            or type(asset.get("decodedSize")) is not int
            or not 0 < asset["decodedSize"] <= 8 * 1024 * 1024):
        raise ValueError("Invalid resource descriptor")


def _read_resource_manifest(source: Path) -> list[dict]:
    if source.stat().st_size > 128 * 1024:
        raise ValueError("Resource manifest exceeds metadata limit")
    manifest = json.loads(source.read_text())
    if manifest.get("schemaVersion") != 1 or not isinstance(manifest.get("assets"), list):
        raise ValueError("Invalid resource manifest")
    return manifest["assets"]


def _add_resource(asset: dict, resources: dict[str, dict], inputs: dict[str, Path]) -> None:
    _validate_resource_descriptor(asset)
    if asset["id"] in resources:
        if resources[asset["id"]] != asset:
            raise ValueError("Conflicting resource identity")
        return
    payload_path = inputs.get(asset["path"])
    if payload_path is None or payload_path.stat().st_size != asset["encodedSize"]:
        raise ValueError("Missing or wrong-size packaged resource")
    if hashlib.sha256(payload_path.read_bytes()).hexdigest() != asset["sha256"]:
        raise ValueError("Packaged resource digest mismatch")
    resources[asset["id"]] = asset


def _resource_manifest(manifests: list[Path], inputs: dict[str, Path]) -> dict:
    resources: dict[str, dict] = {}
    for source in manifests:
        for asset in _read_resource_manifest(source):
            _add_resource(asset, resources, inputs)
    if len(resources) > 128 or sum(asset["encodedSize"] for asset in resources.values()) > 16 * 1024 * 1024:
        raise ValueError("Packaged resources exceed aggregate limit")
    return {"schemaVersion": 1, "assets": sorted(resources.values(), key=lambda asset: asset["id"])}


def _collect_assets(assets: list[tuple[str, Path]], inputs: dict[str, Path]) -> list[Path]:
    manifests = []
    for relative, source in assets:
        if relative == "manifest.json":
            manifests.append(source)
        elif relative in inputs and relative.startswith("assets/"):
            if source.stat().st_size > 8 * 1024 * 1024 or inputs[relative].read_bytes() != source.read_bytes():
                raise ValueError(f"Conflicting resource payload: {relative}")
        else:
            _add_input(inputs, relative, source)
    return manifests


def _check_collisions(inputs: dict[str, Path]) -> None:
    for relative in inputs:
        for parent in PurePosixPath(relative).parents:
            if parent.as_posix() in inputs:
                raise ValueError(f"File/directory collision in web package: {relative}")


def package_files(output: Path, files: list[Path], assets: list[tuple[str, Path]]) -> None:
    """Validate the complete destination map before writing any bundle content."""
    inputs: dict[str, Path] = {}
    for source in files:
        _add_input(inputs, source.name, source)
    manifests = _collect_assets(assets, inputs)
    manifest = _resource_manifest(manifests, inputs) if manifests else None
    if manifest is not None:
        for reserved in ("packaged-resources.json", "packaged-resources.js"):
            if reserved in inputs:
                raise ValueError(f"Duplicate web package path: {reserved}")
    catalog = _compact_catalog_manifest(inputs)
    _check_collisions(inputs)
    _copy_inputs(output, inputs, catalog)
    if manifest is not None:
        serialized = json.dumps(manifest, separators=(",", ":"), sort_keys=True)
        (output / "packaged-resources.json").write_text(serialized + "\n")
        (output / "packaged-resources.js").write_text(
            "globalThis.DonnerPackagedResources = " + serialized + ";\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--file", type=Path, action="append", default=[])
    parser.add_argument("--asset", nargs=2, metavar=("RELATIVE", "SOURCE"), action="append", default=[])
    arguments = parser.parse_args()
    assets = [(relative, Path(source)) for relative, source in arguments.asset]
    package_files(arguments.output, arguments.file, assets)


if __name__ == "__main__":
    main()
