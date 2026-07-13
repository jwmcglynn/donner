#!/usr/bin/env python3
"""Shared fixtures for the runner and adapter-contract tests."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import png_image


SOLID_PIXEL = bytes((10, 20, 30, 255))
OTHER_PIXEL = bytes((200, 100, 50, 255))


def adapter_argv() -> list[str]:
    """The reference adapter as an executable-plus-argument array.

    The adapter is a standalone script invoked as a separate process, mirroring
    how a real adapter is an independent executable.
    """

    adapter = Path(__file__).resolve().with_name("reference_adapter.py")
    return [sys.executable, str(adapter)]


def make_png(path: Path, width: int = 2, height: int = 2, pixel: bytes = SOLID_PIXEL) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png_image.encode_rgba(width, height, pixel * width * height))


def test_entry(
    *,
    test_id: str,
    input_rel: str,
    oracle_rel: str,
    width: int = 2,
    height: int = 2,
    capabilities: list[str] | None = None,
    resources: list[str] | None = None,
) -> dict:
    entry = {
        "id": test_id,
        "input": input_rel,
        "oracle": {
            "kind": "png",
            "path": oracle_rel,
            "width": width,
            "height": height,
            "provenance": "test-fixture",
        },
        "assertion": "fixture assertion",
        "spec_requirements": ["svg2-cr-20181004/painting/paint-order/req-03"],
        "capabilities": capabilities or ["paint-order"],
    }
    if resources is not None:
        entry["resources"] = resources
    return entry


def write_manifest(root: Path, tests: list[dict]) -> Path:
    manifest = {
        "schema": "https://donner.graphics/svg2-suite/corpus-v1.schema.json",
        "corpus": "donner-svg2",
        "revision": "0" * 40,
        "tests": tests,
    }
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def write_profile(root: Path, cases: dict, name: str = "test-profile") -> Path:
    profile = {
        "schema": "https://donner.graphics/svg2-suite/profile-v1.schema.json",
        "profile": name,
        "cases": cases,
    }
    path = root / "profile.json"
    path.write_text(json.dumps(profile), encoding="utf-8")
    return path
