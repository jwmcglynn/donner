# Developer Guide: In-place SVG Editing and Save Support

## Overview
The in-place SVG editing pipeline preserves user formatting while allowing DOM mutations to rewrite
only the spans that changed. Parsing captures source offsets for every relevant token, DOM setters
emit edits targeted at those spans, a planner orders and resolves replacements, and the save API
applies them to the original text while returning updated offset maps.

## Source capture
During XML parse, nodes receive `SourceInfo` records that store byte-range spans into the original
text. Captured spans include:
- Element start tags with per-attribute name, equal-sign, and value spans plus surrounding
  whitespace.
- Text nodes, comments, CDATA sections, processing instructions, and end-tag markers.
- Self-closing markers to distinguish `<tag/>` from `<tag></tag>` forms.

Spans are stored as `[start, end)` offsets and are remapped as edits adjust text length. The
`SourceDocument` owns the immutable original buffer and maintains remapping tables when replacements
are applied.

## DOM mutation hooks
Attribute and text setters emit `EditOperation` instances that target the recorded value span only,
leaving attribute names, spacing, and comments intact. When a node lacks source spans (for example,
newly created elements), the `LocalizedEditBuilder` serializes the node and selects anchors based on
nearby spans:
- Insert-before: anchor at the next sibling's start span.
- Append-child: anchor at the parent's closing tag, deriving indentation from existing children.
- Remove: replace the target span with an empty string while leaving surrounding whitespace
  untouched.

All emitted operations are span-based replacements; structural edits resolve to inserted or removed
text with indentation inferred from neighbors.

## Replace-span planning and application
`ReplaceSpanPlanner` orders edits by start offset, validates that replacements do not overlap, and
promotes operations to a fallback span when necessary (for example, if a referenced span is missing
or conflicts with another edit). The planner produces a conflict-free list of replacements.

`SourceDocument::applyReplacements` executes the ordered edits in one pass using a rope-like helper
that avoids quadratic copies. It builds the new text, verifies expected source ranges, and produces
an offset-translation map so later operations can be expressed against the updated document.

## PathSpline serialization
`PathSpline::ToString` converts spline commands back to SVG path data in two modes controlled by
`PathFormatOptions`:
- **Readable (default):** normalized command letters, helpful separators, and trimmed floats for
  legibility.
- **Size optimized:** removes optional whitespace and separators, drops redundant leading zeros, and
  clamps float precision while preserving visual fidelity.

Path values are regenerated from `PathSpline` overrides during saves; loss of the original token
layout is confined to path data only.

## Save API
`SaveDocument` orchestrates a save for a single source file:
1. Collect pending `EditOperation` entries from DOM mutations (including path overrides formatted
   with the chosen `PathFormatOptions`).
2. Plan replacements via `ReplaceSpanPlanner`, elevating to fallback spans when spans are missing or
   conflicting per policy.
3. Apply the ordered replacements through `SourceDocument::applyReplacements` to produce updated
   text and offset maps.
4. Return `SaveResult` containing the new text, applied replacements, diagnostics, and the updated
   span map for continued editing.

## Whitespace, comments, and diagnostics
Because edits target value spans, surrounding whitespace, entity references, and comments remain
unchanged. Insertions copy indentation from neighboring spans so new nodes blend into existing
formatting. Attribute removals drop only the attribute span, leaving adjacent whitespace as-is.

Structured diagnostics flag overlap conflicts, invalid spans, or planner fallbacks. Tests cover
whitespace-preserving saves, randomized edit orders, large documents, and path-serialization cases to
ensure span stability and minimal diffs.
