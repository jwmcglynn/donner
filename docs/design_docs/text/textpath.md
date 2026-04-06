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

### Custom goldens and thresholds

Any golden or tolerance change requires explicit human approval after visual retriage.

| Tests | Reason |
|-------|--------|
| 001-005 | Minor char advance diffs |
| 009-010 | Minor char advance diffs |
| 011 | AA diffs |
| 013-014 | Minor char advance diffs |
| 015 | AA diffs |
| 019-020 | Minor char advance diffs / AA diffs |
| 022-023 | Minor char advance diffs |
| 026-029 | Minor char advance diffs |
| 032, 035-037 | Minor char advance diffs |
| 034 | AA artifacts (WithThreshold 0.05) |

### Skipped Tests

| Test | Reason |
|------|--------|
| 007 | `method=stretch` not implemented |
| 008 | `spacing=auto` not implemented |
| 012 | Bug: Kerning on textPath |
| 016 | Link to `<rect>` (SVG 2 feature) |
| 021 | `writing-mode=tb` on textPath (deferred) |
| 030 | Vertical text + circular path (deferred) |
| 033 | UB: baseline-shift + rotate interaction |
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

### Open Issues
- [ ] **012**: Kerning on textPath — mixed textPath/tspan/textPath continuation has
  positioning errors in the flat text between path segments.

### Deferred Features
- [ ] Implement `method=stretch` for glyph stretching along path curvature.
- [ ] Implement `spacing=auto` for automatic inter-character spacing on path.
- [ ] Support `side=right` (SVG 2) for text on the opposite side of the path.
- [ ] Support `path` attribute (SVG 2) for inline path data.
- [ ] Handle text across discontinuous subpaths correctly.
- [ ] Support `writing-mode=tb` (vertical text) on textPath.
