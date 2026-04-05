# Text Rendering: textPath

[Back to hub](../text_rendering.md)

**Status:** Shipped for Text v1

## Overview

The `<textPath>` element renders text along an arbitrary path referenced by `href`. Donner
supports the core textPath feature set: basic glyph-on-path positioning, startOffset,
text-anchor, baseline-shift perpendicular to the path, letter-spacing, rotate, and transforms
on the referenced path element.

## Supported Attributes

| Attribute | Type | Default | Status |
|-----------|------|---------|--------|
| `href` / `xlink:href` | `<IRI>` | required | Shipped |
| `startOffset` | `<length>` or `<percentage>` | `0` | Shipped |
| `method` | `align` \| `stretch` | `align` | `align` only; `stretch` deferred |
| `side` | `left` \| `right` | `left` | `left` only; `right` deferred (SVG2) |
| `spacing` | `auto` \| `exact` | `exact` | `exact` only; `auto` deferred |

## Architecture

### Path Resolution (`TextSystem::resolveTextPath`)

When a `<textPath>` span is encountered during `instantiateComputedComponent`:

1. **Resolve href** — Look up the target element via `Reference::resolve()`. If the href is
   empty, the reference is invalid, or the target has no path data, the span is marked
   `textPathFailed = true` and its glyphs are hidden (SVG spec §10.12.1).

2. **Extract path geometry** — Get the `PathSpline` from the target's `ComputedPathComponent`,
   or parse it from the `PathComponent`'s `d` attribute as a fallback.

3. **Apply referenced element transform** — If the referenced `<path>` element has a
   `ComputedLocalTransformComponent`, the path points are transformed into the parent
   coordinate space. This ensures text follows the path as it visually appears.

4. **Resolve startOffset** — Convert the `startOffset` attribute to pixels. Percentages are
   resolved against the (potentially transformed) path length.

5. **Store results** — The resolved `PathSpline` and `pathStartOffset` (in pixels) are stored
   on the `ComputedTextComponent::TextSpan`.

### Glyph Positioning (`TextEngine::layout`)

During the per-span layout loop, spans with `pathSpline` set enter the text-on-path branch:

1. **text-anchor** — Compute total text advance (including kerning and inter-glyph
   letter-spacing) and shift the effective startOffset: `middle` shifts by `-advance/2`,
   `end` by `-advance`. Path-based runs are marked `onPath = true` so the post-loop
   `applyTextAnchor()` skips them (anchor is already applied along the path, not linearly).

2. **baseline-shift** — The combined dominant-baseline and per-span baseline-shift offset
   is applied perpendicular to the path tangent using the direction `(sin(θ), -cos(θ))`.

3. **Per-glyph positioning** — For each glyph:
   - Apply within-run kerning (`xKern`) to the advance accumulator before sampling.
   - Sample the path at `startOffset + advanceAccum + xAdvance/2` to get position and angle.
   - Place the glyph origin shifted back along the tangent by half the advance width.
   - Apply the perpendicular baseline offset.
   - Set rotation to the tangent angle plus any per-character rotation from `rotateList`.
   - Glyphs past the path end are hidden (`glyphIndex = 0`).

4. **Letter-spacing** — Added to the inter-glyph advance accumulation (not after the last
   glyph), so characters spread along the path with correct spacing.

5. **Nested textPath rejection** — `findApplicableTextPathEntity()` stops searching when it
   encounters any `<textPath>` element, even if it's not a valid direct child of `<text>`.
   Content inside a nested (invalid) textPath is marked `textPathFailed` and hidden.

### Error Handling

- **Missing/invalid href**: Span marked `textPathFailed`, glyphs hidden entirely.
- **Empty path**: Same behavior — `resolveTextPath` returns early, span marked failed.
- **Path overflow**: Glyphs past the path end are hidden via `glyphIndex = 0`.

## Test Coverage

The `e-textPath-*` resvg test suite remains enabled, but custom goldens are now restricted to
the human-approved pre-existing set only.

### Human-approved custom goldens

Only these textPath overrides remain approved:

| Tests | Description |
|-------|-------------|
| 001-005 | Basic textPath and startOffset coverage |
| 009-010 | Two-path sequence, nested-invalid textPath handling |

Any additional custom golden or tolerance change requires explicit human approval after live
retriage against the upstream resvg reference images.

### 2026-04-04 retriage after removing post-010 overrides

Command used:

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite -- \
  --gtest_filter='*e_textPath_011:*e_textPath_012:*e_textPath_013:*e_textPath_014:\
*e_textPath_015:*e_textPath_019:*e_textPath_020:*e_textPath_022:*e_textPath_023:\
*e_textPath_025:*e_textPath_026:*e_textPath_027:*e_textPath_028:*e_textPath_029:\
*e_textPath_032:*e_textPath_034:*e_textPath_036:*e_textPath_037'
```

All 18 retriaged cases fail against the upstream resvg references once the unapproved
post-010 overrides are removed.

#### Likely semantic/layout bugs

These still look like real behavior mismatches and should stay red until fixed:

| Test | Pixels | Triage detail |
|------|--------|---------------|
| 011 | 2009 | Mixed content: text before/after a textPath still does not hand off cleanly. |
| 012 | 4477 | Mixed content: `v`/`t` start correctly, but later glyphs drift and `long` spacing is wrong. |
| 015 | 1455 | Long-text overflow/continuation behavior on and after the path is still off. |
| 022 | 604 | `tspan x/y` inside textPath remains path-local positioning work, not threshold work. |
| 023 | 623 | `tspan dx/dy` inside textPath is much closer now, but still a semantic bug. |
| 025 | 3818 | Invalid textPath in mixed content still resumes the surrounding text incorrectly. |
| 028 | 798 | text-decoration on a path still needs real path-following behavior. |

#### Red until human-reviewed, but may be mostly drift

These are smaller diffs that may be backend/raster drift or minor geometry drift, but they still
need visual review and explicit approval before any new golden or tolerance is considered:

| Test | Pixels | Triage detail |
|------|--------|---------------|
| 013 | 759 | Parent `<text>` x/y with textPath. |
| 014 | 630 | Coordinates on `<textPath>` element. |
| 019 | 326 | text-anchor on path. |
| 020 | 397 | Closed circular path. |
| 026 | 242 | ClosePath triangle. |
| 027 | 109 | ClosePath + baseline-shift. |
| 029 | 458 | rotate attribute on path text. |
| 032 | 121 | baseline-shift on path text. |
| 034 | 112 | Arc path sampling. |
| 036 | 664 | Transform on referenced path. |
| 037 | 583 | Transform on ancestor group. |

### Skipped Tests

| Test | Reason |
|------|--------|
| 007 | `method=stretch` not implemented |
| 008 | `spacing=auto` not implemented |
| 016 | Link to `<rect>` (SVG 2 feature) |
| 021 | `writing-mode=tb` on textPath (deferred) |
| 030 | Vertical text + circular path (deferred) |
| 033 | UB: baseline-shift + rotate interaction |
| 035 | `dy` on textPath with 100x transform scaling |
| 040 | Filter on textPath not implemented |
| 041 | `side=right` (SVG 2 feature) |
| 042 | `path` attribute (SVG 2 feature) |
| 043-044 | `path` + `href` interaction (SVG 2) |

## Key Files

| Component | Path |
|-----------|------|
| DOM element | `donner/svg/SVGTextPathElement.h` |
| ECS component | `donner/svg/components/text/TextPathComponent.h` |
| Computed data | `donner/svg/components/text/ComputedTextComponent.h` (TextSpan fields) |
| Path resolution | `donner/svg/components/text/TextSystem.cc` (`resolveTextPath`) |
| Glyph layout | `donner/svg/text/TextEngine.cc` (text-on-path section) |
| Attribute parser | `donner/svg/parser/AttributeParser.cc` (textPath specialization) |

## Future Work

### Bug Fixes (v1 blockers)
- [ ] Fix mixed-content continuation bugs in `011`, `012`, `015`, and `025`.
- [ ] Finish path-local `tspan` positioning for `022` and `023`.
- [ ] Implement path-following text decoration for `028`.
- [ ] Re-review `013`, `014`, `019`, `020`, `026`, `027`, `029`, `032`, `034`, `036`, and `037`
  before considering any new golden or tolerance.

### Deferred Features
- [ ] Implement `method=stretch` for glyph stretching along path curvature.
- [ ] Implement `spacing=auto` for automatic inter-character spacing on path.
- [ ] Support `side=right` (SVG 2) for text on the opposite side of the path.
- [ ] Support `path` attribute (SVG 2) for inline path data.
- [ ] Handle text across discontinuous subpaths correctly.
- [ ] Support `writing-mode=tb` (vertical text) on textPath.
