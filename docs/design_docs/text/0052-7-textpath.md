# Text Rendering: textPath

[Back to hub](../0010-text_rendering.md)

**Status:** Shipped for Text v1
**Author:** Claude Opus 4.6

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

2. **Extract path geometry** — Get the `Path` from the target's `ComputedPathComponent`,
   or parse it from the `PathComponent`'s `d` attribute as a fallback.

3. **Apply referenced element transform** — If the referenced `<path>` element has a
   `ComputedLocalTransformComponent`, the path points are transformed into the parent
   coordinate space. This ensures text follows the path as it visually appears.

4. **Resolve startOffset** — Convert the `startOffset` attribute to pixels. Percentages are
   resolved against the (potentially transformed) path length.

5. **Store results** — The resolved `Path` and `pathStartOffset` (in pixels) are stored
   on the `ComputedTextComponent::TextSpan`.

### Glyph Positioning (`TextEngine::layout`)

During the per-span layout loop, spans with `pathData` set enter the text-on-path branch:

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

### Character endpoints

Character endpoint queries accumulate a path cluster's signed advance vectors from its first
alignment point. The direction includes the writing mode, authored rotation, and path rotation;
zero-advance marks retain the cluster endpoint. Both aligned and stretched text use the placed
advance direction, including when the outline follows a bend.

`//donner/svg/tests:svg_tests` and its `_text_full` and `_geode` variants cover vertical and
sloped paths, authored rotation, upright CJK with combining marks, and a contour-corner control
for `getEndPositionOfChar`. The controls also retain unrotated flat-text behavior and authored
textLength checks.

### Error Handling

- **Missing/invalid href**: Span marked `textPathFailed`, glyphs hidden entirely.
- **Empty path**: Same behavior — `resolveTextPath` returns early, span marked failed.
- **Path overflow**: Glyphs past the path end are hidden via `glyphIndex = 0`.

## Test Coverage

The `e-textPath-*` resvg test suite remains enabled, and custom goldens are restricted to the
human-approved set.

### Custom goldens and thresholds

Any golden or tolerance change requires explicit human approval after visual retriage. The 300x300
upstream resvg references, byte-identical to the resvg v0.50.0 release, use default comparison
settings on the canvas each case takes from its reference. Their provenance is in
`third_party/resvg-test-suite/BUILD.bazel`.

| Test | Setting | Reason |
|------|---------|--------|
| closed-path | golden (CPU), resvg v0.50.0 golden (Geode), default settings | The release reference inverts arc length to within 0.5 user units (golden README) |
| dy-with-tiny-coordinates | golden, default settings | resvg 0.47.0 reference (golden README) |
| method=stretch | golden | The snapshot depicts align placement. The Donner golden guards glyph warping. |
| mixed-children-1, mixed-children-2, very-long-text | golden | SVG 2 continues text at the path end point. |
| side=right | threshold 0.05 | Half-turned glyph edges |
| tspan-with-absolute-position | golden | Absolute distance along the text path |
| with-underline | golden | Underline paint along the text path |
| with-filter | golden, default settings | The reference draws glyph edges 0.25 to 0.3 px outside the outline |
| writing-mode=tb | golden, default settings, 300x300 | The reference moves upright glyphs off the font's vmtx origin |

For the opaque `with-path` and `with-path-and-xlink-href` cases, see the
[fixture coverage matrix](../../spec_audit/test_coverage.md#text).

### Render-only cases

| Test | Disposition |
| --- | --- |
| complex | Render only. The PNG is the suite's undefined-behavior marker. |
| with-baseline-shift-and-rotate | Render only. The interaction has no defined result in this fixture. |

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
- [x] **012**: Mixed textPath/tspan/textPath continuation follows the SVG 2 text layout
  algorithm: unanchored text after each path starts at that path's geometric endpoint. The
  historical reference instead continues from the occupied text extent, so the row compares
  against a Donner golden whose ink matches Chrome within one device pixel.
  Independent renderer controls compare both continuations against explicit endpoint positions in
  `//donner/svg/renderer/tests:text_path_convergence_tests` and its `_geode` and `_text_full` variants.

### Deferred Features
- [x] Implement `method=stretch` for glyph stretching along path curvature.
- [ ] Implement `spacing=auto` for automatic inter-character spacing on path.
- [ ] Support `side=right` (SVG 2) for text on the opposite side of the path.
- [ ] Support `path` attribute (SVG 2) for inline path data.
- [ ] Handle text across discontinuous subpaths correctly.
- [x] Map vertical inline advances, glyph offsets, rotation, and textLength into the path frame.
  `//donner/svg/renderer/tests:text_engine_scripted_tests` covers upright glyph offsets, cluster
  rotation, and textLength. `//donner/svg/renderer/tests:text_path_convergence_tests` and its
  `_geode` and `_text_full` variants compare the straight-path vertical layout and textLength
  against ordinary vertical text, with visible-ink negative controls.
- [x] Resolve upright CJK glyph-origin differences between text backends. Both backends place
  upright glyphs at the font's vertical origin (glyph box top plus vmtx top side bearing) with the
  vmtx advance, so each glyph's vertical advance cell is centered on the path at its advance
  midpoint. `//donner/svg/renderer/tests:text_path_convergence_tests` and its `_geode` and
  `_text_full` variants compare flat and on-path upright CJK against horizontal text placed at
  the vmtx-derived origins. The refreshed `writing-mode=tb.svg` reference (upstream quoted its
  `'Mplus 1p'` family) places upright glyphs off the vmtx origin, so the row compares against a
  Donner golden at default settings (see the golden README).
