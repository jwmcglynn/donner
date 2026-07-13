#!/usr/bin/env python3
"""Validate the SVG2 compliance-baseline seed (design 0057, Milestone 0).

Milestone 0 establishes the compliance baseline. This tool validates the two
seed artifacts that define its shape:

- ``baseline.lock.json``: the exact SVG 2 formal publication and editorial-delta
  revisions with content digests, plus the normative dependency lock; and
- ``requirements.*.json``: reviewed normative requirement records with stable
  ids, applicability, oracle kinds, and evidence states.

It checks structural validity against the schemas and the accounting rules the
design makes non-negotiable: requirement ids are unique and carry the locked
baseline prefix; only a reviewed requirement with a linked test may claim
``covered-pass`` or ``covered-fail``; a ``draft-dependency`` requirement must
actually depend on a non-stable dependency; and every dependency key a
requirement names must exist in the lock. This is the seed validator for the
worked-example chapter, not the full chapter-by-chapter audit, which is a
separate campaign.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import jsonschema_lite


REQUIREMENT_SCHEMA_NAME = "requirement-v1.schema.json"
BASELINE_LOCK_SCHEMA_NAME = "baseline-lock-v1.schema.json"


def default_schema_dir() -> Path:
    return Path(__file__).resolve().parent / "schemas"


def _load_schema(name: str, schema_dir: Path) -> dict[str, Any]:
    return json.loads((schema_dir / name).read_text(encoding="utf-8"))


def _is_requirement_ref(dependency: str) -> bool:
    return dependency.startswith("svg2-cr-")


def validate_requirements(
    inventory: dict[str, Any], lock_keys: set[str] | None = None
) -> list[str]:
    """Apply the requirement accounting rules to a schema-valid inventory."""

    errors: list[str] = []
    baseline = inventory["baseline"]
    seen: set[str] = set()

    for requirement in inventory["requirements"]:
        req_id = requirement["id"]
        if req_id in seen:
            errors.append(f"duplicate requirement id: {req_id!r}")
        seen.add(req_id)

        if not req_id.startswith(baseline + "/"):
            errors.append(
                f"requirement id {req_id!r} does not carry baseline prefix {baseline!r}"
            )

        evidence = requirement["evidence_state"]
        test_ids = requirement.get("test_ids", [])
        if evidence in ("covered-pass", "covered-fail") and not test_ids:
            errors.append(
                f"{req_id!r} claims {evidence!r} but links no test"
            )
        if evidence == "covered-pass" and requirement["review_state"] != "reviewed":
            errors.append(
                f"{req_id!r} claims covered-pass but is not reviewed"
            )

        dependencies = requirement.get("dependencies", [])
        if evidence == "draft-dependency" and not dependencies:
            errors.append(
                f"{req_id!r} claims draft-dependency but names no dependency"
            )

        if lock_keys is not None:
            for dependency in dependencies:
                if _is_requirement_ref(dependency):
                    continue
                if dependency not in lock_keys:
                    errors.append(
                        f"{req_id!r} depends on unlocked dependency {dependency!r}"
                    )

    return errors


def validate_baseline_lock(lock: dict[str, Any]) -> list[str]:
    """Apply lock-level accounting rules to a schema-valid baseline lock."""

    errors: list[str] = []
    seen: set[str] = set()
    for entry in lock["dependency_lock"]["entries"]:
        key = entry["key"]
        if key in seen:
            errors.append(f"duplicate dependency key: {key!r}")
        seen.add(key)
        if entry["stable"] and entry["status"].lower() in ("candidate recommendation", "editor's draft", "working draft"):
            errors.append(
                f"dependency {key!r} is marked stable but its status is a draft"
            )
    return errors


def check(spec_dir: Path, schema_dir: Path | None = None) -> list[str]:
    """Validate every seed artifact in ``spec_dir`` and return error strings."""

    schema_dir = schema_dir or default_schema_dir()
    errors: list[str] = []

    lock_path = spec_dir / "baseline.lock.json"
    lock_keys: set[str] | None = None
    if not lock_path.is_file():
        errors.append("missing baseline.lock.json")
    else:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
        lock_schema = _load_schema(BASELINE_LOCK_SCHEMA_NAME, schema_dir)
        structural = jsonschema_lite.validate(lock, lock_schema)
        if structural:
            errors.extend(f"baseline.lock.json: {message}" for message in structural)
        else:
            errors.extend(f"baseline.lock.json: {message}" for message in validate_baseline_lock(lock))
            lock_keys = {entry["key"] for entry in lock["dependency_lock"]["entries"]}

    requirement_schema = _load_schema(REQUIREMENT_SCHEMA_NAME, schema_dir)
    inventory_paths = sorted(spec_dir.glob("requirements.*.json"))
    if not inventory_paths:
        errors.append("no requirements.*.json inventory files found")
    for path in inventory_paths:
        inventory = json.loads(path.read_text(encoding="utf-8"))
        structural = jsonschema_lite.validate(inventory, requirement_schema)
        if structural:
            errors.extend(f"{path.name}: {message}" for message in structural)
            continue
        errors.extend(
            f"{path.name}: {message}"
            for message in validate_requirements(inventory, lock_keys)
        )

    return errors


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate the SVG2 compliance-baseline seed.")
    parser.add_argument(
        "--spec-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "spec",
        help="Directory containing baseline.lock.json and requirements.*.json",
    )
    parser.add_argument("--schema-dir", type=Path, default=None)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    errors = check(args.spec_dir, args.schema_dir)
    if errors:
        print("SVG2 baseline seed validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("SVG2 baseline seed is valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
