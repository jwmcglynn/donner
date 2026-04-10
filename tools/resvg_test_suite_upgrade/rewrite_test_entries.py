#!/usr/bin/env python3
"""Rewrite resvg_test_suite.cc for the new resvg-test-suite layout.

Consumes:
  - overrides.json (from extract_overrides.py)
  - rename_map.json (from build_rename_map.py)

Produces:
  - resvg_test_suite.cc.new   — draft rewritten source
  - migration_report.md       — migrated / orphaned / ambiguous / low-confidence
                                counts and per-entry details

The new file uses `getTestsInCategory("painting/fill", { ... })` style discovery
grouped by the category directory tree of the new layout. Suites that cross
multiple directories in the new layout (because a prefix like `a-fill-` now
scatters into painting/fill, painting/color, masking/mask, etc.) get split.
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from dataclasses import dataclass


@dataclass(frozen=True)
class NewEntry:
    old_filename: str        # "a-fill-010.svg"
    new_path: str            # "tests/painting/fill/rgb-int-int-int.svg"
    new_category: str        # "painting/fill"
    new_filename: str        # "rgb-int-int-int.svg"
    params: str              # raw Params expression
    comment: str


def map_old_to_new(
    old_filename: str,
    rename_map: dict[str, str | None],
) -> str | None:
    """Look up "a-fill-010.svg" in the rename map keyed by "svg/a-fill-010.svg"."""
    key = f"svg/{old_filename}"
    return rename_map.get(key)


def split_new_path(new_path: str) -> tuple[str, str]:
    """Split 'tests/painting/fill/rgb-int-int-int.svg' -> ('painting/fill', 'rgb-int-int-int.svg')."""
    assert new_path.startswith("tests/"), new_path
    rest = new_path[len("tests/"):]
    parts = rest.rsplit("/", 1)
    if len(parts) != 2:
        return ("", rest)
    return (parts[0], parts[1])


def sanitize_suite_name(category: str) -> str:
    """`painting/fill-opacity` → `PaintingFillOpacity` for INSTANTIATE_TEST_SUITE_P."""
    pieces = re.split(r"[/_-]", category)
    return "".join(p[:1].upper() + p[1:] for p in pieces if p)


# Custom golden override files have names like "resvg-a-fill-010.png". In the
# source these can be expressed as a single string literal or as adjacent
# C++ string literals (implicit concatenation) — handle both.
#
# Matches `Params::WithGoldenOverride("..." "..." ...)` with one or more
# adjacent string literals, capturing the literals span so we can replace it.
GOLDEN_OVERRIDE_RE = re.compile(
    r'Params::WithGoldenOverride\(\s*((?:"[^"]*"\s*)+)(?:,[^)]*)?\)'
)


def rewrite_golden_override(
    params: str, old_filename: str, new_filename: str,
) -> tuple[str, str | None]:
    """If params contains WithGoldenOverride("..."), rewrite the filename
    stem from resvg-<old> to resvg-<new>. Handles adjacent-literal
    concatenation. Returns (new_params, new_path_or_None).
    """
    m = GOLDEN_OVERRIDE_RE.search(params)
    if not m:
        return params, None
    literals_span = m.group(1)
    # Concatenate the adjacent literals into a single path.
    concatenated = "".join(re.findall(r'"([^"]*)"', literals_span))
    old_stem = old_filename[:-4] if old_filename.endswith(".svg") else old_filename
    new_stem = new_filename[:-4] if new_filename.endswith(".svg") else new_filename
    new_path = concatenated.replace(f"resvg-{old_stem}", f"resvg-{new_stem}")
    # Replace the adjacent-literal span with a single canonical literal.
    new_params = (
        params[:m.start(1)] + f'"{new_path}"' + params[m.end(1):]
    )
    return new_params, new_path


# Match factory calls where we can inject a reason argument as the last positional arg.
# Keep this conservative — only rewrite the exact forms we know the codemod emits.
SKIP_RE = re.compile(r"Params::Skip\(\)")
RENDER_ONLY_RE = re.compile(r"Params::RenderOnly\(\)")
WITH_THRESHOLD_ONE_ARG_RE = re.compile(r"Params::WithThreshold\(\s*([^,)]+?)\s*\)")
WITH_THRESHOLD_TWO_ARG_RE = re.compile(
    r"Params::WithThreshold\(\s*([^,)]+?)\s*,\s*([^,)]+?)\s*\)")


def escape_reason(text: str) -> str:
    """Escape a comment string for embedding in a C++ string literal."""
    return text.replace("\\", "\\\\").replace('"', '\\"')


def inject_reason(params: str, comment: str) -> str:
    """Rewrite `Params::Skip()` / `::RenderOnly()` / `::WithThreshold(...)` to
    include the comment as a trailing reason string argument. Other forms
    (chained builders starting from `Params()`) get a `.withReason("...")`
    call appended. If no comment, return params unchanged.
    """
    if not comment:
        return params

    reason = escape_reason(comment)

    # Prefer the factory-arg form for the canonical cases.
    if SKIP_RE.search(params) and "Skip()" in params:
        return SKIP_RE.sub(f'Params::Skip("{reason}")', params, count=1)
    if RENDER_ONLY_RE.search(params) and "RenderOnly()" in params:
        return RENDER_ONLY_RE.sub(f'Params::RenderOnly("{reason}")', params, count=1)

    # WithThreshold(threshold) → WithThreshold(threshold, kDefaultMismatchedPixels, "reason")
    # WithThreshold(threshold, max) → WithThreshold(threshold, max, "reason")
    m2 = WITH_THRESHOLD_TWO_ARG_RE.search(params)
    if m2:
        return params[:m2.start()] + (
            f'Params::WithThreshold({m2.group(1)}, {m2.group(2)}, "{reason}")'
        ) + params[m2.end():]
    m1 = WITH_THRESHOLD_ONE_ARG_RE.search(params)
    if m1:
        return params[:m1.start()] + (
            f'Params::WithThreshold({m1.group(1)}, kDefaultMismatchedPixels, "{reason}")'
        ) + params[m1.end():]

    # Everything else (WithGoldenOverride, chained Params() builders) — append
    # .withReason("...") so the reason lands on the final object. This works
    # regardless of how the object was constructed.
    return params + f'.withReason("{reason}")'


def build_new_suites(
    overrides: list[dict],
    rename_map: dict[str, str | None],
) -> tuple[
    dict[str, list[NewEntry]],  # category → entries
    list[dict],                  # orphaned (deleted upstream)
    list[dict],                  # ambiguous (inline comment problems)
    list[dict],                  # golden renames (old → new path)
]:
    by_category: dict[str, list[NewEntry]] = defaultdict(list)
    orphaned: list[dict] = []
    ambiguous: list[dict] = []
    golden_renames: list[dict] = []

    for o in overrides:
        old_filename = o["filename"]
        new_path = map_old_to_new(old_filename, rename_map)
        if new_path is None:
            orphaned.append({
                "old_filename": old_filename,
                "params": o["params"],
                "comment": o["comment"],
                "suite_name": o["suite_name"],
            })
            continue

        category, new_filename = split_new_path(new_path)

        # Handle golden override renames.
        new_params, renamed_golden = rewrite_golden_override(
            o["params"], old_filename, new_filename)
        if renamed_golden:
            golden_renames.append({
                "old": GOLDEN_OVERRIDE_RE.search(o["params"]).group(1),
                "new": renamed_golden,
            })

        # Flag entries whose params expression contains '//' (inline comments
        # that got smushed by the extractor) — these need manual cleanup.
        if "//" in new_params:
            ambiguous.append({
                "old_filename": old_filename,
                "new_path": new_path,
                "params": new_params,
                "comment": o["comment"],
            })

        by_category[category].append(NewEntry(
            old_filename=old_filename,
            new_path=new_path,
            new_category=category,
            new_filename=new_filename,
            params=new_params,
            comment=o["comment"],
        ))

    return by_category, orphaned, ambiguous, golden_renames


def emit_source(
    by_category: dict[str, list[NewEntry]],
    all_categories: list[str] | None = None,
) -> str:
    """Emit a draft resvg_test_suite.cc body with new INSTANTIATE_TEST_SUITE_P
    blocks, one per directory in sorted order.

    The entries inside each suite are sorted by new filename for stability.
    This is a DRAFT — it assumes the file header, getTestsInCategory helper,
    and TEST_P function already exist elsewhere in the file. The caller must
    splice this output in place of the old INSTANTIATE_TEST_SUITE_P section.

    If all_categories is provided, emit suites for every category in that list
    (in addition to the ones with overrides), leaving the override map empty
    for uncovered categories. This ensures every category in the new test tree
    gets an INSTANTIATE_TEST_SUITE_P so its tests actually run.
    """
    lines: list[str] = []
    lines.append("// AUTOGENERATED DRAFT by tools/resvg_test_suite_upgrade/rewrite_test_entries.py.")
    lines.append("// Hand-review required: regrouping, inline-comment ambiguities, and new tests.")
    lines.append("")

    categories_to_emit: set[str] = set(by_category.keys())
    if all_categories:
        categories_to_emit.update(all_categories)

    for category in sorted(categories_to_emit):
        entries = sorted(by_category.get(category, []), key=lambda e: e.new_filename)
        suite_name = sanitize_suite_name(category)

        if not entries:
            # No overrides in this category — emit a one-liner default suite.
            lines.append(f"INSTANTIATE_TEST_SUITE_P({suite_name}, ImageComparisonTestFixture,")
            lines.append(f'                         ValuesIn(getTestsInCategory("{category}")),')
            lines.append(f"                         TestNameFromFilename);")
            lines.append("")
            continue

        lines.append(f"INSTANTIATE_TEST_SUITE_P(")
        lines.append(f"    {suite_name}, ImageComparisonTestFixture,")
        lines.append(f'    ValuesIn(getTestsInCategory("{category}",')
        lines.append(f"                                {{")
        for e in entries:
            # Escape embedded double quotes in new_filename (new layout uses
            # descriptive names that can include unusual chars).
            escaped = e.new_filename.replace("\\", "\\\\").replace('"', '\\"')
            params_with_reason = inject_reason(e.params, e.comment)
            entry_line = (
                f'                                    {{"{escaped}", {params_with_reason}}},'
            )
            lines.append(entry_line)
        lines.append(f"                                }})),")
        lines.append(f"    TestNameFromFilename);")
        lines.append("")

    return "\n".join(lines) + "\n"


def emit_report(
    overrides: list[dict],
    by_category: dict[str, list[NewEntry]],
    orphaned: list[dict],
    ambiguous: list[dict],
    golden_renames: list[dict],
) -> str:
    total = len(overrides)
    migrated = sum(len(v) for v in by_category.values())
    orph = len(orphaned)
    amb = len(ambiguous)

    lines: list[str] = []
    lines.append("# resvg-test-suite upgrade: migration report")
    lines.append("")
    lines.append(f"- Total overrides in old file: **{total}**")
    lines.append(f"- Successfully migrated:        **{migrated}**")
    lines.append(f"- Orphaned (deleted upstream):  **{orph}**")
    lines.append(f"- Ambiguous (inline comments):  **{amb}**")
    lines.append(f"- Golden overrides renamed:     **{len(golden_renames)}**")
    lines.append("")
    lines.append(f"- Distinct new categories: **{len(by_category)}**")
    lines.append("")

    lines.append("## Per-category entry counts")
    lines.append("")
    for cat in sorted(by_category.keys()):
        lines.append(f"- `{cat}`: {len(by_category[cat])} entries")
    lines.append("")

    lines.append("## Orphaned entries (require manual decision)")
    lines.append("")
    if not orphaned:
        lines.append("_none_")
    else:
        for o in orphaned:
            lines.append(
                f"- `{o['old_filename']}` — `{o['params']}` // {o['comment']} "
                f"(was in {o['suite_name']})")
    lines.append("")

    lines.append("## Ambiguous entries (inline comments in params)")
    lines.append("")
    if not ambiguous:
        lines.append("_none_")
    else:
        for a in ambiguous:
            lines.append(f"- `{a['old_filename']}` → `{a['new_path']}`")
            lines.append(f"  - params: `{a['params']}`")
            if a['comment']:
                lines.append(f"  - comment: {a['comment']}")
    lines.append("")

    lines.append("## Golden override renames")
    lines.append("")
    if not golden_renames:
        lines.append("_none_")
    else:
        for g in golden_renames:
            lines.append(f"- `{g['old']}` → `{g['new']}`")
    lines.append("")

    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--overrides", type=Path, required=True)
    parser.add_argument("--rename-map", type=Path, required=True)
    parser.add_argument("--all-categories", type=Path, default=None,
                        help="Optional file listing every category dir (one per "
                             "line, relative to tests/). Empty suites are "
                             "emitted for any category without overrides.")
    parser.add_argument("--out-source", type=Path, required=True)
    parser.add_argument("--out-report", type=Path, required=True)
    args = parser.parse_args()

    overrides_data = json.loads(args.overrides.read_text())
    rename_data = json.loads(args.rename_map.read_text())

    overrides = overrides_data["overrides"]
    rename_map = rename_data["mapping"]

    by_category, orphaned, ambiguous, golden_renames = build_new_suites(
        overrides, rename_map)

    all_categories: list[str] | None = None
    if args.all_categories:
        all_categories = [
            line.strip()
            for line in args.all_categories.read_text().splitlines()
            if line.strip()
        ]

    source = emit_source(by_category, all_categories=all_categories)
    report = emit_report(overrides, by_category, orphaned, ambiguous, golden_renames)

    args.out_source.parent.mkdir(parents=True, exist_ok=True)
    args.out_source.write_text(source)
    args.out_report.parent.mkdir(parents=True, exist_ok=True)
    args.out_report.write_text(report)

    total = len(overrides)
    migrated = sum(len(v) for v in by_category.values())
    print(f"Migrated: {migrated}/{total}", file=sys.stderr)
    print(f"Orphaned: {len(orphaned)}", file=sys.stderr)
    print(f"Ambiguous: {len(ambiguous)}", file=sys.stderr)
    print(f"Categories: {len(by_category)}", file=sys.stderr)
    print(f"Wrote {args.out_source} and {args.out_report}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
