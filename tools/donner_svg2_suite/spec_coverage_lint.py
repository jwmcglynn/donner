#!/usr/bin/env python3
"""Coverage lint for the SVG2 requirement inventory (design 0057).

The requirement inventory is the source of truth for SVG 2 compliance. Imported
and authored tests link to requirement ids; missing requirements stay visible as
gaps; and skips never count as coverage. This lint enforces the bidirectional
requirement<->test graph and the non-negotiable accounting rules the design's
"Full Normative Compliance Audit" section makes explicit, then emits the three
audit artifacts (``requirements.json``, ``coverage.json``, ``gaps.json``).

It complements ``spec_seed`` (baseline/dependency accounting) and
``manifest_validation`` (per-corpus structural + path safety). This tool owns the
cross-cutting graph between inventories and corpus manifests:

- every requirement carries a known evidence state;
- requirement ids are globally unique;
- every corpus test maps only to requirement ids that exist (no unknown ids);
- an extension-corpus test must map at least one requirement (no orphans);
- coverage edges are mutual: if a test lists a requirement, the requirement
  lists the test, and vice versa;
- a requirement that claims coverage (``covered-pass`` / ``covered-fail``) must
  link a test that actually exists in a corpus; a ``missing-test`` requirement
  must not silently already have a mapped test;
- ``covered-pass`` requires a reviewed requirement with a real linked test, so a
  skip, an unsupported feature, or an untested requirement can never be counted
  as passing evidence (the "no skip-as-pass accounting" rule);
- an id group (baseline/chapter/anchor) may not carry drifting section/anchor
  text (stale-anchor detection); and
- a single test may not map requirements whose processing modes are disjoint
  (a mapping across incompatible conformance modes).

Only ``covered-pass`` contributes passing compliance evidence. ``covered-fail``,
``missing-test``, ``unsupported``, ``not-directly-testable``, ``spec-ambiguity``,
and ``draft-dependency`` remain visible gaps. ``not-applicable`` is excluded from
the denominator but requires a reviewed rationale so a profile cannot use it to
hide an implemented-but-wrong feature.

Security: this tool reads suite-controlled JSON only, through the size-capped,
path-safe readers in ``path_safety`` and ``manifest_validation``. It never opens
a manifest-declared corpus file, executes anything, or touches the network.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from manifest_validation import validate_manifest
from path_safety import read_text_capped
import jsonschema_lite


REQUIREMENT_SCHEMA_NAME = "requirement-v1.schema.json"

# Evidence-state partitions. Only ``covered-pass`` is passing evidence; every
# other testable state is a visible gap. ``not-applicable`` leaves the
# denominator entirely and must cite a reviewed rationale.
PASSING_STATES = frozenset({"covered-pass"})
GAP_STATES = frozenset(
    {
        "covered-fail",
        "missing-test",
        "unsupported",
        "not-directly-testable",
        "spec-ambiguity",
        "draft-dependency",
    }
)
NOT_APPLICABLE_STATES = frozenset({"not-applicable"})
ALL_EVIDENCE_STATES = PASSING_STATES | GAP_STATES | NOT_APPLICABLE_STATES

# States that assert a real test exists and demonstrates behavior. A requirement
# in one of these states must link at least one test that a corpus defines.
COVERAGE_CLAIMING_STATES = frozenset({"covered-pass", "covered-fail"})

# Corpora whose membership is not, by itself, a requirement mapping. Imported
# base cases (design 0057, "Oracle governance") legitimately carry no mapping
# until reviewed, so they are not treated as orphans.
DEFAULT_BASE_CORPORA = frozenset({"resvg"})


def default_schema_dir() -> Path:
    return Path(__file__).resolve().parent / "schemas"


def _id_group(requirement_id: str) -> tuple[str, str, str] | None:
    """Return (baseline, chapter, anchor-slug) for a requirement id, or None."""

    parts = requirement_id.split("/")
    if len(parts) != 4:
        return None
    baseline, chapter, anchor_slug, _ordinal = parts
    return baseline, chapter, anchor_slug


def collect_requirements(inventories: list[dict[str, Any]]) -> tuple[dict[str, dict], list[str]]:
    """Index requirements by id across every inventory, flagging duplicates."""

    by_id: dict[str, dict] = {}
    errors: list[str] = []
    for inventory in inventories:
        for requirement in inventory.get("requirements", []):
            req_id = requirement["id"]
            if req_id in by_id:
                errors.append(f"duplicate requirement id: {req_id!r}")
                continue
            by_id[req_id] = requirement
    return by_id, errors


def collect_tests(corpora: list[dict[str, Any]], base_corpora: frozenset[str]) -> tuple[dict[str, dict], list[str]]:
    """Index tests by id across every corpus manifest, flagging duplicates.

    Each indexed record carries the mapped requirement ids, the owning corpus,
    and whether that corpus requires every test to map a requirement.
    """

    by_id: dict[str, dict] = {}
    errors: list[str] = []
    for corpus in corpora:
        name = corpus.get("corpus", "")
        requires_mapping = name not in base_corpora
        for test in corpus.get("tests", []):
            test_id = test["id"]
            if test_id in by_id:
                errors.append(f"duplicate test id across corpora: {test_id!r}")
                continue
            by_id[test_id] = {
                "corpus": name,
                "requirements": list(test.get("spec_requirements", [])),
                "requires_mapping": requires_mapping,
            }
    return by_id, errors


def _check_anchor_consistency(requirements_by_id: dict[str, dict]) -> list[str]:
    """Detect drifting section/anchor text within a baseline/chapter/anchor group."""

    errors: list[str] = []
    groups: dict[tuple[str, str, str], tuple[str, str]] = {}
    for req_id, requirement in sorted(requirements_by_id.items()):
        group = _id_group(req_id)
        if group is None:
            continue
        seen = (requirement.get("anchor", ""), requirement.get("section", ""))
        if group in groups and groups[group] != seen:
            errors.append(
                f"stale anchor for id group {'/'.join(group)}: {req_id!r} carries "
                f"anchor/section {seen!r} but a sibling recorded {groups[group]!r}"
            )
        groups.setdefault(group, seen)
    return errors


def lint(
    inventories: list[dict[str, Any]],
    corpora: list[dict[str, Any]] | None = None,
    *,
    base_corpora: frozenset[str] = DEFAULT_BASE_CORPORA,
) -> list[str]:
    """Return coverage-graph and accounting errors for the given inputs.

    ``inventories`` are requirement-v1 inventory documents; ``corpora`` are
    corpus-v1 manifest documents. Passing ``corpora=None`` lints inventory-only
    rules (duplicates, states, anchor drift, covered-pass integrity) and treats a
    listed test id as a not-yet-authored candidate.
    """

    corpora = corpora or []
    requirements_by_id, errors = collect_requirements(inventories)
    tests_by_id, test_errors = collect_tests(corpora, base_corpora)
    errors.extend(test_errors)
    errors.extend(_check_anchor_consistency(requirements_by_id))

    for req_id in sorted(requirements_by_id):
        requirement = requirements_by_id[req_id]
        evidence = requirement.get("evidence_state")
        if evidence not in ALL_EVIDENCE_STATES:
            errors.append(f"{req_id!r} has unknown or missing evidence state: {evidence!r}")
            continue

        review_state = requirement.get("review_state")
        test_ids = requirement.get("test_ids", [])

        # Forward links: requirement -> test.
        for test_id in test_ids:
            record = tests_by_id.get(test_id)
            if record is None:
                if evidence in COVERAGE_CLAIMING_STATES:
                    errors.append(
                        f"{req_id!r} claims {evidence!r} but its linked test "
                        f"{test_id!r} exists in no corpus"
                    )
                # Otherwise the id is a recorded candidate for a not-yet-authored
                # test, which the design permits for a gap requirement.
                continue
            if req_id not in record["requirements"]:
                errors.append(
                    f"non-mutual coverage edge: {req_id!r} lists test {test_id!r} "
                    f"but that test does not list the requirement"
                )
            if evidence == "missing-test":
                errors.append(
                    f"{req_id!r} is marked missing-test but its linked test "
                    f"{test_id!r} already exists in corpus {record['corpus']!r}"
                )

        # Evidence-state integrity: the no-skip-as-pass rules.
        if evidence == "covered-pass":
            if review_state != "reviewed":
                errors.append(f"{req_id!r} claims covered-pass but is not reviewed")
            if not test_ids:
                errors.append(f"{req_id!r} claims covered-pass but links no test")
        if evidence == "covered-fail" and not test_ids:
            errors.append(f"{req_id!r} claims covered-fail but links no test")
        if evidence in NOT_APPLICABLE_STATES:
            if review_state != "reviewed":
                errors.append(f"{req_id!r} is not-applicable but is not reviewed")
            if not requirement.get("rationale", "").strip():
                errors.append(
                    f"{req_id!r} is not-applicable but records no rationale "
                    f"(a conformance rule must justify exclusion)"
                )

    for test_id in sorted(tests_by_id):
        record = tests_by_id[test_id]
        mapped = record["requirements"]
        if record["requires_mapping"] and not mapped:
            errors.append(
                f"orphaned test {test_id!r} in corpus {record['corpus']!r}: "
                f"an extension case must map at least one requirement"
            )

        modes: list[set[str]] = []
        for req_id in mapped:
            requirement = requirements_by_id.get(req_id)
            if requirement is None:
                errors.append(
                    f"test {test_id!r} maps unknown requirement id {req_id!r}"
                )
                continue
            if test_id not in requirement.get("test_ids", []):
                errors.append(
                    f"non-mutual coverage edge: test {test_id!r} maps requirement "
                    f"{req_id!r} but that requirement does not list the test"
                )
            modes.append(set(requirement.get("processing_modes", [])))

        if len(modes) >= 2 and not set.intersection(*modes):
            errors.append(
                f"test {test_id!r} maps requirements with disjoint processing modes; "
                f"a single case cannot satisfy incompatible conformance modes"
            )

    return errors


def build_artifacts(inventories: list[dict[str, Any]]) -> dict[str, dict]:
    """Build the requirements/coverage/gaps audit artifacts from inventories.

    Accounting is deliberately non-interchangeable: ``covered-pass`` is the only
    passing state, every other testable state is a gap, and ``not-applicable``
    leaves the denominator. This is the machine-readable embodiment of the
    no-skip-as-pass rule.
    """

    requirements_by_id, _ = collect_requirements(inventories)
    baseline = inventories[0]["baseline"] if inventories else ""

    merged = [requirements_by_id[req_id] for req_id in sorted(requirements_by_id)]
    requirements_doc = {
        "schema": "https://donner.graphics/svg2-suite/requirement-v1.schema.json",
        "baseline": baseline,
        "requirements": merged,
    }

    requirement_to_tests: dict[str, list[str]] = {}
    test_to_requirements: dict[str, list[str]] = {}
    for req_id in sorted(requirements_by_id):
        linked = sorted(requirements_by_id[req_id].get("test_ids", []))
        requirement_to_tests[req_id] = linked
        for test_id in linked:
            test_to_requirements.setdefault(test_id, [])
            if req_id not in test_to_requirements[test_id]:
                test_to_requirements[test_id].append(req_id)
    for test_id in test_to_requirements:
        test_to_requirements[test_id] = sorted(test_to_requirements[test_id])

    coverage_doc = {
        "baseline": baseline,
        "requirement_to_tests": requirement_to_tests,
        "test_to_requirements": test_to_requirements,
    }

    gaps = []
    passing = 0
    not_applicable = 0
    for req_id in sorted(requirements_by_id):
        requirement = requirements_by_id[req_id]
        evidence = requirement["evidence_state"]
        if evidence in PASSING_STATES:
            passing += 1
        elif evidence in NOT_APPLICABLE_STATES:
            not_applicable += 1
        else:
            gaps.append(
                {
                    "id": req_id,
                    "evidence_state": evidence,
                    "section": requirement.get("section", ""),
                    "anchor": requirement.get("anchor", ""),
                    "rationale": requirement.get("rationale", ""),
                }
            )

    total = len(requirements_by_id)
    gaps_doc = {
        "baseline": baseline,
        "summary": {
            "total": total,
            "covered_pass": passing,
            "gaps": len(gaps),
            "not_applicable": not_applicable,
        },
        "gaps": gaps,
    }

    return {
        "requirements": requirements_doc,
        "coverage": coverage_doc,
        "gaps": gaps_doc,
    }


def _load_inventories(spec_dir: Path, schema_dir: Path) -> tuple[list[dict], list[str]]:
    schema = json.loads((schema_dir / REQUIREMENT_SCHEMA_NAME).read_text(encoding="utf-8"))
    inventories: list[dict] = []
    errors: list[str] = []
    paths = sorted(spec_dir.glob("requirements.*.json"))
    if not paths:
        errors.append("no requirements.*.json inventory files found")
    for path in paths:
        inventory = json.loads(read_text_capped(path))
        structural = jsonschema_lite.validate(inventory, schema)
        if structural:
            errors.extend(f"{path.name}: {message}" for message in structural)
            continue
        inventories.append(inventory)
    return inventories, errors


def _load_corpora(
    corpus_paths: list[Path], schema_dir: Path, known_requirements: set[str]
) -> tuple[list[dict], list[str]]:
    corpora: list[dict] = []
    errors: list[str] = []
    for path in corpus_paths:
        structural = validate_manifest(path, schema_dir, known_requirements=known_requirements)
        if structural:
            errors.extend(f"{path.name}: {message}" for message in structural)
            continue
        corpora.append(json.loads(read_text_capped(path)))
    return corpora, errors


def check(
    spec_dir: Path,
    corpus_paths: list[Path] | None = None,
    schema_dir: Path | None = None,
    *,
    base_corpora: frozenset[str] = DEFAULT_BASE_CORPORA,
) -> list[str]:
    """Validate the on-disk inventories and corpora, returning error strings."""

    schema_dir = schema_dir or default_schema_dir()
    inventories, errors = _load_inventories(spec_dir, schema_dir)
    if errors:
        return errors

    known_requirements = {
        requirement["id"]
        for inventory in inventories
        for requirement in inventory["requirements"]
    }
    corpora, corpus_errors = _load_corpora(corpus_paths or [], schema_dir, known_requirements)
    errors.extend(corpus_errors)
    if errors:
        return errors

    return lint(inventories, corpora, base_corpora=base_corpora)


def emit_artifacts(spec_dir: Path, out_dir: Path, schema_dir: Path | None = None) -> list[str]:
    """Write requirements/coverage/gaps artifacts from the on-disk inventories."""

    schema_dir = schema_dir or default_schema_dir()
    inventories, errors = _load_inventories(spec_dir, schema_dir)
    if errors:
        return errors
    artifacts = build_artifacts(inventories)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, document in artifacts.items():
        (out_dir / f"{name}.json").write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return []


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Lint the SVG2 requirement/test coverage graph.")
    parser.add_argument(
        "--spec-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "spec",
        help="Directory containing requirements.*.json inventory files.",
    )
    parser.add_argument(
        "--corpus",
        type=Path,
        action="append",
        default=None,
        help="Corpus manifest.json to cross-check (repeatable).",
    )
    parser.add_argument("--schema-dir", type=Path, default=None)
    parser.add_argument(
        "--emit-dir",
        type=Path,
        default=None,
        help="When set, write requirements.json/coverage.json/gaps.json here.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    errors = check(args.spec_dir, args.corpus, args.schema_dir)
    if errors:
        print("SVG2 coverage lint failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    if args.emit_dir is not None:
        emit_errors = emit_artifacts(args.spec_dir, args.emit_dir, args.schema_dir)
        if emit_errors:
            for error in emit_errors:
                print(f"  {error}", file=sys.stderr)
            return 1
        print(f"SVG2 coverage graph is consistent; artifacts written to {args.emit_dir}.")
    else:
        print("SVG2 coverage graph is consistent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
