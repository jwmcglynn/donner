#!/usr/bin/env python3
"""Validate a Donner SVG2 test-suite corpus manifest.

A corpus manifest (``corpus/<name>/manifest.json``) records *facts* about
tests: stable IDs, inputs, typed oracles, spec-requirement links, capabilities,
and resources. This validator enforces the rules from design 0057 so a manifest
cannot silently ship a broken or unsafe corpus:

- structural validity against ``schemas/corpus-v1.schema.json`` (this also
  rejects an unsupported schema version and malformed requirement IDs);
- globally unique test IDs;
- path safety for every referenced path (no ``..``, symlinks, absolute paths,
  or URLs) checked before any file is opened;
- every referenced path rooted in its declared corpus subtree
  (``tests/`` for inputs and PNG oracles, ``resources/`` or ``fonts/`` for
  resources), so nothing implicit or out-of-tree is pulled in;
- every referenced file present on disk; and
- optional content-integrity hashes: when the manifest carries an ``integrity``
  map, recorded ``sha256`` digests must match the files on disk.

When the caller supplies a set of known requirement IDs, the validator also
rejects ``spec_requirements`` that name an unknown requirement. The full
requirement/coverage graph is a separate campaign (design 0057, Milestone 0),
so that cross-check is optional here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

from path_safety import (
    DEFAULT_MAX_BYTES,
    UnsafePathError,
    ensure_relative,
    read_text_capped,
    resolve_within_root,
)
import jsonschema_lite


CORPUS_SCHEMA_NAME = "corpus-v1.schema.json"

# Declared corpus subtrees. A path referenced by a test must live under one of
# the roots allowed for its role; anything else is an undeclared, out-of-tree
# reference.
INPUT_ROOTS = ("tests/",)
RESOURCE_ROOTS = ("resources/", "fonts/")

_SHA256_HEX_LENGTH = 64


def default_schema_dir() -> Path:
    return Path(__file__).resolve().parent / "schemas"


def load_schema(name: str, schema_dir: Path | None = None) -> dict[str, Any]:
    directory = schema_dir or default_schema_dir()
    return json.loads((directory / name).read_text(encoding="utf-8"))


def _under_any_root(relative: str, roots: tuple[str, ...]) -> bool:
    return any(relative.startswith(root) for root in roots)


def _hash_file(path: str, max_bytes: int = DEFAULT_MAX_BYTES) -> str:
    digest = hashlib.sha256()
    total = 0
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            total += len(chunk)
            if total > max_bytes:
                raise UnsafePathError(f"file exceeds the {max_bytes}-byte hash cap: {path!r}")
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def _check_referenced_path(
    corpus_root: Path,
    relative: str,
    roots: tuple[str, ...],
    role: str,
    errors: list[str],
) -> str | None:
    """Validate one referenced path and return its absolute form, or None."""

    try:
        ensure_relative(relative)
    except UnsafePathError as error:
        errors.append(f"unsafe {role} path: {error}")
        return None

    if not _under_any_root(relative, roots):
        allowed = " or ".join(roots)
        errors.append(
            f"undeclared {role} path {relative!r}: must be rooted under {allowed}"
        )
        return None

    try:
        absolute = resolve_within_root(corpus_root, relative)
    except UnsafePathError as error:
        errors.append(f"unsafe {role} path: {error}")
        return None

    if not Path(absolute).is_file():
        errors.append(f"missing {role} file: {relative!r}")
        return None

    return absolute


def _check_integrity(
    corpus_root: Path, integrity: dict[str, Any], errors: list[str]
) -> None:
    for relative, expected in integrity.items():
        try:
            absolute = resolve_within_root(corpus_root, relative)
        except UnsafePathError as error:
            errors.append(f"unsafe integrity path: {error}")
            continue
        if not Path(absolute).is_file():
            errors.append(f"missing integrity file: {relative!r}")
            continue
        try:
            actual = _hash_file(absolute)
        except UnsafePathError as error:
            errors.append(str(error))
            continue
        if actual != expected:
            errors.append(
                f"hash mismatch for {relative!r}: expected {expected}, got {actual}"
            )


def validate_manifest(
    manifest_path: Path,
    schema_dir: Path | None = None,
    known_requirements: set[str] | None = None,
    corpus_root: Path | None = None,
) -> list[str]:
    """Validate the manifest at ``manifest_path`` and return error strings.

    An empty list means the manifest is valid. Structural errors are reported on
    their own; when the shape is broken, the deeper filesystem checks are
    skipped because they assume a well-formed manifest.

    By default, referenced paths (``tests/...``, ``resources/...``, ...) are
    resolved relative to the manifest file's own parent directory. Passing an
    explicit ``corpus_root`` overrides that and resolves referenced paths
    against a different directory instead. This lets a manifest that was
    generated into its own location (for example a temporary file produced by
    a generator tool) be validated against the actual corpus tree it
    describes, without copying or symlinking that tree next to the manifest.
    """

    resolved_root = Path(corpus_root) if corpus_root is not None else manifest_path.resolve().parent
    try:
        raw = read_text_capped(manifest_path)
    except (OSError, UnsafePathError) as error:
        return [f"cannot read manifest: {error}"]
    try:
        manifest = json.loads(raw)
    except json.JSONDecodeError as error:
        return [f"manifest is not valid JSON: {error}"]

    schema = load_schema(CORPUS_SCHEMA_NAME, schema_dir)
    structural = jsonschema_lite.validate(manifest, schema)
    if structural:
        return structural

    errors: list[str] = []
    tests = manifest["tests"]

    seen_ids: set[str] = set()
    for test in tests:
        test_id = test["id"]
        if test_id in seen_ids:
            errors.append(f"duplicate test id: {test_id!r}")
        seen_ids.add(test_id)

        _check_referenced_path(resolved_root, test["input"], INPUT_ROOTS, "input", errors)

        oracle = test["oracle"]
        if oracle.get("kind") == "png":
            _check_referenced_path(resolved_root, oracle["path"], INPUT_ROOTS, "oracle", errors)

        for resource in test.get("resources", []):
            _check_referenced_path(resolved_root, resource, RESOURCE_ROOTS, "resource", errors)

        if known_requirements is not None:
            for requirement in test["spec_requirements"]:
                if requirement not in known_requirements:
                    errors.append(
                        f"unknown requirement id in {test_id!r}: {requirement!r}"
                    )

    integrity = manifest.get("integrity")
    if isinstance(integrity, dict):
        _check_integrity(resolved_root, integrity, errors)

    return errors


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate an SVG2 suite corpus manifest.")
    parser.add_argument("manifest", type=Path, help="Path to corpus manifest.json")
    parser.add_argument(
        "--schema-dir",
        type=Path,
        default=None,
        help="Directory containing the corpus schema (defaults beside this tool)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    errors = validate_manifest(args.manifest, args.schema_dir)
    if errors:
        print(f"Manifest validation failed for {args.manifest}:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(f"Manifest {args.manifest} is valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
