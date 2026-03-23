# Text Rendering: textPath Implementation Plan

[Back to hub](../text_rendering.md)

## `<textPath>` Implementation Plan (v0.5)

The `<textPath>` element renders text along an arbitrary path referenced by `href`. This is
the last text-related element needed for v0.5 (currently listed as "Not yet supported" in README).

### SVGTextPathElement Class

Create `donner/svg/SVGTextPathElement.h` extending `SVGTextPositioningElement`:

- Add `ElementType::TextPath` to the enum in `ElementType.h`.
- Register in `AllSVGElements.h` type list.
- Register tag `"textPath"` in `SVGParser::createElement()`.

**Attributes:**

| Attribute | Type | Default | Description |
|-----------|------|---------|-------------|
| `href` / `xlink:href` | `<IRI>` | required | Reference to a `<path>` element |
| `startOffset` | `<length>` or `<percentage>` | `0` | Offset along the path where text begins |
| `method` | `align` \| `stretch` | `align` | How glyphs are placed on the path |
| `side` | `left` \| `right` | `left` | Which side of the path to render text |
| `textLength` | `<length>` | auto | Desired text length for adjustment |
| `lengthAdjust` | `spacing` \| `spacingAndGlyphs` | `spacing` | How to adjust to `textLength` |
| `spacing` | `auto` \| `exact` | `exact` | Spacing control (SVG2) |

### Path-Based Text Layout

Extend `TextSystem` / `ComputedTextComponent` to handle path-based glyph positioning:

1. **Resolve path reference** — Look up the `href` target, which must be a `<path>` element.
   Extract its `PathSpline` geometry.

2. **Compute path length** — Use `PathSpline::pathLength()` to get total arc length.
   Resolve `startOffset` against this length (percentages are relative to path length).

3. **Sample path at glyph positions** — For each glyph:
   - Compute the distance along the path: `startOffset + sum of prior glyph advances`.
   - Sample the path at that distance to get `(x, y)` position and tangent angle.
   - Position the glyph midpoint at `(x, y)` with rotation = tangent angle.
   - If `method="stretch"`, additionally scale each glyph along the tangent direction
     to match the path curvature.

4. **Handle `side="right"`** — Reverse the path direction before sampling. Glyphs are
   placed on the opposite side and rendered upside-down relative to the path.

5. **Handle overflow** — Glyphs that extend past the end of the path are hidden. If the
   path is closed, text wraps only if the UA supports it (typically not).

### Path Sampling Algorithm

```
function samplePathAtDistance(spline, distance):
  // Walk segments until cumulative length >= distance
  for each segment in spline:
    segLength = segment.arcLength()
    if distance <= segLength:
      t = segment.parameterForArcLength(distance)
      point = segment.evaluate(t)
      tangent = segment.tangent(t)
      return (point, atan2(tangent.y, tangent.x))
    distance -= segLength
  return null  // past end of path
```

The `PathSpline` already has `pathLength()` and segment evaluation. We need to add
`parameterForArcLength()` — binary search on the segment's arc length integral.

### Renderer Integration

Both backends need to render glyphs with per-glyph transforms:

- **Current approach**: `drawText` renders glyph outlines as paths. Each glyph already gets
  individual path data from font metrics.
- **Extension**: For `<textPath>` glyphs, prepend a per-glyph rotation+translation transform
  before drawing each glyph outline. This requires the text layout to output per-glyph
  transforms alongside the glyph paths.

### Tests

- Enable resvg `e-textPath-*` test suite (currently `// TODO(text): e-textPath`).
- Unit tests for path sampling, startOffset, method, side attributes.
- Edge cases: zero-length paths, single-point paths, very long text overflowing path.
