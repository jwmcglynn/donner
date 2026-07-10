#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


AUTHOR_RE = re.compile(r"^\*\*Author:\*\*\s*(.+?)\\?\s*$", re.MULTILINE)
MODEL_RE = re.compile(r"^\*\*Model:\*\*\s*(.+?)\\?\s*$", re.MULTILINE)
HUMAN_ONLY_RE = re.compile(
    r"^\*\*Model:\*\*\s*None \(human-only\)\\?\s*$", re.MULTILINE
)
EXACT_MODEL_RES = (
    re.compile(r"^GPT-[0-9]+(?:\.[0-9]+)*(?: [A-Za-z][A-Za-z0-9.-]*)*$"),
    re.compile(r"^Claude [A-Za-z][A-Za-z0-9.-]* [0-9]+(?:\.[0-9]+)*$"),
)
IGNORED_FILENAMES = {
    "AGENTS.md",
    "README.md",
    "design_template.md",
    "developer_template.md",
    "retrospective_template.md",
}
APPROVED_HISTORICAL_DEBT = frozenset(
    {
        "docs/design_docs/0001-terminal_image_viewer.md",
        "docs/design_docs/0002-mcp_test_triage_server.md",
        "docs/design_docs/0004-external_svg_references.md",
        "docs/design_docs/0006-color_emoji.md",
        "docs/design_docs/0007-coverage_improvement_plan.md",
        "docs/design_docs/0008-css_fonts.md",
        "docs/design_docs/0009-resvg_test_suite_bugs.md",
        "docs/design_docs/0011-v0_5_release.md",
        "docs/design_docs/0013-coverage_improvement.md",
        "docs/design_docs/0014-filter_performance.md",
        "docs/design_docs/0015-skia_filter_conformance.md",
        "docs/design_docs/0017-geode_renderer.md",
        "docs/design_docs/0018-bcr_release.md",
        "docs/design_docs/0020-editor.md",
        "docs/design_docs/0021-resvg_feature_gaps.md",
        "docs/design_docs/0022-resvg_test_suite_upgrade.md",
        "docs/design_docs/0038-geode_tinyskia_text_parity.md",
        "docs/design_docs/0041-geode_analytical_aa.md",
        "docs/design_docs/0042-geode_slug_conformance.md",
        "docs/design_docs/0043-deterministic_replay_testing.md",
        "docs/design_docs/0044-2-editor_fluid_canvas_rendering.md",
        "docs/design_docs/0045-editor_geode_chrome_migration.md",
        "docs/design_docs/0046-editor_group_layers.md",
        "docs/design_docs/0047-v0_8_showcase.md",
    }
)


@dataclass(frozen=True)
class Violation:
    path: str
    message: str


def is_exact_model_identifier(value: str) -> bool:
    return "Codex" not in value and any(pattern.match(value) for pattern in EXACT_MODEL_RES)


def is_design_doc(path: Path) -> bool:
    if path.name in IGNORED_FILENAMES:
        return False
    return bool(re.match(r"^[0-9]{4}(?:-|\b)", path.name)) or path.name == "animation.md"


def validate_document(path: Path, root: Path) -> Violation | None:
    relative_path = path.relative_to(root).as_posix()
    content = path.read_text(encoding="utf-8")
    author_match = AUTHOR_RE.search(content)
    if author_match is None:
        return Violation(relative_path, "missing **Author:** metadata")

    author = author_match.group(1).strip()
    if "Codex" in author:
        return Violation(
            relative_path,
            f"generic surface name in Author metadata: {author!r}",
        )

    if is_exact_model_identifier(author):
        return None

    model_match = MODEL_RE.search(content)
    if model_match is not None and is_exact_model_identifier(model_match.group(1).strip()):
        return None

    if HUMAN_ONLY_RE.search(content):
        return None

    return Violation(
        relative_path,
        f"Author is not an exact model identifier and is not marked human-only: {author!r}",
    )


def collect_violations(root: Path) -> dict[str, Violation]:
    design_root = root / "docs" / "design_docs"
    return {
        violation.path: violation
        for path in sorted(design_root.rglob("*.md"))
        if is_design_doc(path)
        if (violation := validate_document(path, root)) is not None
    }


def load_debt_allowlist(path: Path) -> set[str]:
    entries: set[str] = set()
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        entry = line.strip()
        if not entry or entry.startswith("#"):
            continue
        if entry in entries:
            raise ValueError(f"{path}:{line_number}: duplicate entry {entry!r}")
        entries.add(entry)
    return entries


def check(root: Path, allowlist_path: Path) -> list[str]:
    violations = collect_violations(root)
    debt = load_debt_allowlist(allowlist_path)
    errors = [
        f"{path}: unapproved provenance-debt entry; new debt is prohibited"
        for path in sorted(debt - APPROVED_HISTORICAL_DEBT)
    ]
    errors.extend(
        f"{violation.path}: {violation.message}"
        for path, violation in sorted(violations.items())
        if path not in debt
    )
    errors.extend(
        f"{path}: stale provenance-debt entry; remove it from {allowlist_path.relative_to(root)}"
        for path in sorted(debt - violations.keys())
    )
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Require exact model provenance on Donner design documents."
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Donner repository root",
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        help="Historical provenance-debt file (defaults under docs/design_docs)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    allowlist = args.allowlist or root / "docs" / "design_docs" / "provenance_debt.txt"
    try:
        errors = check(root, allowlist.resolve())
    except (OSError, ValueError) as error:
        print(f"design provenance check failed: {error}", file=sys.stderr)
        return 1

    if errors:
        print("Design document provenance violations:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print("Design document provenance check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
