# Design: SVG Static-Subset Completion Roadmap

**Status:** Draft
**Author:** Claude Opus 4.8
**Created:** 2026-07-13

## Summary

This is a planning document, not an implementation. It inventories what remains for Donner to fully
render the SVG 2 **static subset**: the elements, attributes, and features needed to display a
non-animated, non-scripted SVG faithfully. It is derived from a direct audit of the engine and of
the conformance run in `donner/svg/renderer/tests/resvg_test_suite.cc`, and it is organized so each
gap becomes a follow-on implementation slice.

The measurement backbone is [design 0057](0057-donner_svg2_test_suite.md), the portable SVG 2 test
suite and normative requirement inventory. Where 0057's requirement inventory or generated gap
report becomes the single source of truth, this roadmap should be reconciled against it rather than
maintained as a parallel list; until then, the roadmap plus the resvg suite skip/expected-fail
comments and the README feature matrix are the interim record.

Scope note: Donner already renders the large majority of the static subset. The core shape, paint,
gradient, pattern, marker, clip, mask, and filter-primitive machinery works on both the CPU
(tiny_skia) and GPU (Geode) backends. The remaining work is concentrated in text, external resource
loading, a set of masking/clipping and filter edge cases, and CPU/GPU renderer parity.

## Goals

- Enumerate every unsupported or partial feature that is in scope for **full static rendering**, and
  group it into deliverable slices with a clear "done" signal (a resvg suite case or a 0057
  requirement moving from skipped/expected-fail to passing).
- Keep the roadmap honest: each item names the observed gap and its ground-truth evidence, so no
  slice is marked complete on aspiration.
- Sequence the work by user-visible impact and by unblocking value (fix shared machinery before
  edge cases).

## Non-Goals

- Implementing the gaps. Each milestone is a separate future slice with its own design/PR.
- Animation (SMIL), scripting, DOM events, and interaction beyond hyperlinks. Link hit-testing is
  already delivered via `DonnerController::hitTestLink`; further interaction is out of scope here.
- SVG 1.1 features that SVG 2 removed or deprecated, which are not part of the SVG 2 static subset:
  `enable-background` / `BackgroundImage` / `BackgroundAlpha` filter inputs, `<tref>`, CSS2
  `clip: rect(...)`, SVG fonts (`<font>` / `<glyph>`), and `<altGlyph*>`. These stay documented as
  intentionally unsupported (see `docs/unsupported_svg1_features.md`).
- Conic/sweep gradients (not part of the SVG core static subset).

## Next Steps

- Reconcile this inventory against the design 0057 normative requirement IDs as that inventory lands,
  and attach a requirement ID to each roadmap item.
- Start with Milestone 1 (text completion), the largest and most user-visible cluster.

## Implementation Plan

- [ ] Milestone 1: Text static-subset completion (largest gap cluster)
  - [ ] Bidirectional text: `direction`, `unicode-bidi`, and bidi reordering.
  - [ ] `textLength` and `lengthAdjust` (`spacing` and `spacingAndGlyphs`).
  - [ ] SVG 2 `<textPath>` features: `side`, `method=stretch`, `spacing=auto`, the `path` attribute,
        and reference-to-shape targets.
  - [ ] Full SVG 2 `text-decoration` (independent line, style, and color).
  - [ ] Vertical text: remaining `writing-mode: tb` cases and `glyph-orientation-*`.
  - [ ] `font-size-adjust`, `font-kerning`, and the `font` shorthand.
- [ ] Milestone 2: External resource loading
  - [ ] External-URL `<image>` references (with the security limits in the Security section).
  - [ ] `<use>` referencing an external file.
  - [ ] External CSS `@import`.
  - [ ] `<svg>` with no explicit size: compute bounds from content.
- [ ] Milestone 3: Masking and clipping edge cases
  - [ ] `clip-path` on and with `<text>`.
  - [ ] Nested clip-path intersection (currently a GPU-backend TODO; align both backends).
  - [ ] Remaining `mask-type` / `maskUnits` / `maskContentUnits` edge cases and
        `color-interpolation=linearRGB` on masks.
- [ ] Milestone 4: Filter completion
  - [ ] CSS `filter:` function-list support (the currently disabled `filter-functions` category).
  - [ ] feImage subregion cases and feConvolveMatrix / feDropShadow edge cases.
  - [ ] Filter-region scissor per SVG 2 section 15.5 (GPU backend TODO).
- [ ] Milestone 5: Painting and structural edge cases
  - [ ] `image-rendering: pixelated` / `crisp-edges` sampling.
  - [ ] `<svg version="1.1">` compatibility handling.
  - [ ] Non-UTF-8 document encodings.
- [ ] Milestone 6: CPU/GPU (Geode) render parity
  - [ ] `paint-order` fill/stroke/marker reordering on the GPU backend.
  - [ ] `0 N` dash round/square caps on the GPU backend.
  - [ ] Stroke-join defect (issue #663).
  - [ ] Color/bitmap-emoji glyphs in the GPU `drawText` path.

## Background

Donner tracks conformance continuously against the upstream resvg test suite with visual regression
in CI. The resvg run is the most reliable signal of real gaps: a category renamed `DISABLED_` via
`Params::Skip`, or a golden override with a "Not impl" comment, marks a concrete unimplemented
feature. This roadmap reads those markers as the backlog and folds them into the broader SVG 2
conformance program defined in [design 0026](0026-svg_conformance_testing.md) and operationalized in
[design 0057](0057-donner_svg2_test_suite.md).

## Proposed Architecture

This document proposes no new architecture. Each milestone extends existing subsystems: the text
engine and layout for Milestone 1; the resource/loader and document-bounds paths for Milestone 2;
the clip/mask systems for Milestone 3; the filter graph executor and both filter backends for
Milestone 4; the parser and image sampling for Milestone 5; and the Geode backend for Milestone 6.
Each slice should land behind the same DOM and renderer-interface boundaries already in place, so
the two backends stay at parity.

## Security / Privacy

Most milestones operate on already-parsed, in-memory document state and add no new trust boundary.
Milestone 2 (external resource loading) is the exception and is the security-sensitive slice: it
introduces fetching of external references (`<image>` URLs, external `<use>` and `@import` targets),
which is untrusted-input handling. Its own design must define the fetch trust boundary before
implementation: default-deny external fetches unless the embedder opts in, no automatic network
access for `file://` or cross-origin targets without explicit configuration, size and time limits,
protection against reference cycles and decompression bombs, and no leakage of local paths. Until
that design exists, external loading stays off. The parser surfaces added by other milestones remain
covered by Donner's continuous fuzzing.

## Testing and Validation

Each slice is validated by moving its resvg suite case(s) from skipped/expected-fail to passing, and
by attaching the corresponding design 0057 normative requirement ID so the compliance report
reflects the change. New behavior adds focused unit tests and, where a parsing surface is touched,
fuzz coverage. A milestone is "done" only when its ground-truth conformance cases pass on both the
CPU and GPU backends (or the divergence is documented as an intentional backend limitation).

## Open Questions

- The order of Milestones 2 through 5 is by estimated impact; the design 0057 requirement inventory
  may reprioritize based on which requirements are mandatory versus optional for a static-subset
  conformance claim.
- Whether external resource loading (Milestone 2) belongs in the static-subset "full support" bar at
  all, or is better treated as an opt-in embedder capability given its security surface.

## Future Work

- [ ] Attach design 0057 requirement IDs to every roadmap item once the normative inventory is
      published, and replace this interim list with a derived view of the 0057 gap report.
