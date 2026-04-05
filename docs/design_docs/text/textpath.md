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

1. **text-anchor** — Compute total text advance (including inter-glyph letter-spacing) and
   shift the effective startOffset: `middle` shifts by `-advance/2`, `end` by `-advance`.

2. **baseline-shift** — The combined dominant-baseline and per-span baseline-shift offset
   is applied perpendicular to the path tangent using the direction `(sin(θ), -cos(θ))`.

3. **Per-glyph positioning** — For each glyph:
   - Sample the path at `startOffset + advanceAccum + xAdvance/2` to get position and angle.
   - Place the glyph origin shifted back along the tangent by half the advance width.
   - Apply the perpendicular baseline offset.
   - Set rotation to the tangent angle plus any per-character rotation from `rotateList`.
   - Glyphs past the path end are hidden (`glyphIndex = 0`).

4. **Letter-spacing** — Added to the inter-glyph advance accumulation (not after the last
   glyph), so characters spread along the path with correct spacing.

### Error Handling

- **Missing/invalid href**: Span marked `textPathFailed`, glyphs hidden entirely.
- **Empty path**: Same behavior — `resolveTextPath` returns early, span marked failed.
- **Path overflow**: Glyphs past the path end are hidden via `glyphIndex = 0`.

## Test Coverage

The `e-textPath-*` resvg test suite covers 44 SVG test cases:

- **33 active tests** — all passing in both base and `text_full` tiers.
- **11 skipped tests** — deferred or out-of-scope features.

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

### Known Gaps (with thresholds)

Some tests pass with elevated thresholds due to implementation gaps:

- **tspan positioning on textPath** (022, 023): `x`/`y`/`dx`/`dy` on tspan children of
  textPath are not fully applied along/perpendicular to the path.
- **Nested/mixed textPath** (010, 012): Multiple textPath elements or mixed text/textPath
  children have interaction gaps.
- **Coords on text** (013): `x`/`y` on the parent `<text>` element interaction with textPath.
- **Subpaths** (024, 025): Text across discontinuous path segments (multiple `M` commands).

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

- [ ] Implement `method=stretch` for glyph stretching along path curvature.
- [ ] Implement `spacing=auto` for automatic inter-character spacing on path.
- [ ] Support `side=right` (SVG 2) for text on the opposite side of the path.
- [ ] Support `path` attribute (SVG 2) for inline path data.
- [ ] Apply `dx`/`dy` on tspan children as along-path / perpendicular offsets.
- [ ] Handle text across discontinuous subpaths correctly.
- [ ] Support `writing-mode=tb` (vertical text) on textPath.
