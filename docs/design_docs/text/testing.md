# Text Rendering: Testing and Validation

[Back to hub](../text_rendering.md)

## Testing and Validation

### Unit tests

- `FontManager` tests: load TTF/OTF/WOFF1, verify `stbtt_fontinfo` initialization, test
  cascade fallback, verify font metrics.
- `TextLayout` tests: lay out known strings with embedded Public Sans, verify glyph positions
  against expected values, test `text-anchor` adjustment.
- `Woff2Parser` tests: decompress known WOFF2 files, verify output matches reference TTF.
- Glyph outline tests: extract outlines for known glyphs, verify path geometry matches expected
  curves.

### Golden image tests

- Add text-specific SVG test cases to `renderer_tests`:
  - Basic Latin text.
  - Multi-span positioning (dx/dy).
  - `text-anchor` (start/middle/end).
  - `@font-face` with embedded WOFF1 font.
  - `@font-face` with WOFF2 font (requires `text_woff2`).
  - Stroke + fill text.
  - Per-glyph rotation.
- Run existing resvg text test subset (currently skipped) and tighten thresholds.

### Feature-gated test skipping

```cpp
{"text_basic.svg", Params::RequiresFeature(Feature::Text)},
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextWoff2)},
```

CI runs all feature combinations to ensure both enabled and disabled paths work.

### Backend parity

- Text golden images differ between Skia and TinySkia at the base tier because Skia uses
  its own internal layout (with GPOS access) while TinySkia uses stb_truetype (kern-table only).
- At the `text_shaping` tier, shaping output is identical between backends since both use
  HarfBuzz, and golden images should be much closer.
- TinySkia renders glyph outlines as vector paths (no hinting), so small-size text will look
  slightly different from Skia's hinted rasterization regardless of shaping tier.

## Current Test Failure Analysis (2026-03-21) {#test-failures}

Snapshot of resvg test suite failures for both base and `--config=text-full` configurations.
Tests that pass in both configs are omitted.

### Categorized Failures

#### 1. Font size rendering (both configs, high diffs)

| Test | Base | text-full | Root Cause |
|------|------|-----------|------------|
| a-font-size-003 | 34397 / 20500 | 34413 / 20500 | Font size scaling for `em`/`ex` units |
| a-font-size-006 | 34308 / 25000 | 34303 / 25000 | Font size with viewport units |
| a-font-size-007 | 39492 / 17000 | 39494 / 17000 | Font size `larger`/`smaller` keywords |
| a-font-size-012 | 23675 / 17000 | 23669 / 17000 | Inherited font size |
| a-font-size-014 | 26994 / 17000 | 27007 / 17000 | Font size on nested elements |
| a-font-size-015 | 18603 / 17000 | 18605 / 17000 | Font size percentage |
| a-font-size-016 | 26994 / 17000 | 27007 / 17000 | Font size with tspan |
| a-font-size-017 | 18603 / 17000 | 18605 / 17000 | Font size with text-anchor |

**Analysis**: All have ~identical pixel counts between configs, indicating the issue is in font size
resolution, not in the shaping tier. Likely CSS `font-size` keyword or relative unit resolution.

#### 2. textPath rendering (both configs)

All `e-textPath-*` tests fail in both configs with default threshold (100 max). Counts range from
112 to 44241 pixels. The textPath implementation has known gaps:

- **Path sampling accuracy**: Glyph mid-point placement along the path may differ from resvg's
  arc-length parameterization.
- **startOffset**: Percentage vs absolute offset handling.
- **textLength on textPath**: Not implemented.
- **Closed paths / overflow**: Glyphs past the path end should be hidden.
- **BIDI on textPath**: Not considered.

The `e-textPath-035` test has 44K pixels (text-full) — likely a complete layout failure for that
specific case. Most tests are in the 500-7000 range, suggesting positioning offsets rather than
completely wrong rendering.

#### 3. Writing mode (both configs)

| Test | Base | text-full | Root Cause |
|------|------|-----------|------------|
| a-writing-mode-010 | 14562 / 10000 | 13253 / 10000 | Vertical text glyph rotation |
| a-writing-mode-012 | passes | 14380 / 12600 | text-full only: vertical CJK |
| a-writing-mode-013 | 3286 / 2900 | passes | base only: vertical Latin |
| a-writing-mode-015 | passes | 2512 / 2000 | text-full only: vertical mixed script |
| a-writing-mode-020 | 13491 / 10100 | 10804 / 10100 | Vertical text-anchor |

**Analysis**: Vertical writing mode has different behavior between stb_truetype (base) and
HarfBuzz (text-full) due to different vertical metric handling. Some tests pass in one config
but fail in the other.

#### 4. Letter spacing (both configs)

| Test | Base | text-full |
|------|------|-----------|
| a-letter-spacing-009 | 3055 / 2600 | 3070 / 2600 |
| a-letter-spacing-011 | passes | 5443 / 2900 |

**Analysis**: Letter spacing interacts differently with HarfBuzz kerning vs stb_truetype kerning.

#### 5. tspan positioning

| Test | Base | text-full |
|------|------|-----------|
| e-tspan-027 | 16870 / 14000 | 16873 / 14000 |

**Analysis**: Complex tspan positioning with multiple nested tspans and absolute/relative coords.

### Text-full only failures (not in base)

- `a-letter-spacing-011`: HarfBuzz GPOS kerning interacts with letter-spacing differently
- `a-writing-mode-012`, `a-writing-mode-015`: Vertical text metrics differ between FreeType and
  stb_truetype

### Base only failures (not in text-full)

- `a-writing-mode-013`: stb_truetype vertical text handling
- `e-textPath-039`: textPath rendering difference

### Summary

| Category | Count | Severity | Next Steps |
|----------|-------|----------|------------|
| Font size | 8 | High (17K-39K px) | Fix CSS font-size keyword/unit resolution |
| textPath | ~30 | Medium-High | Incremental improvements to path sampling |
| tspan | 21 | Medium-High | See tspan gap analysis below |
| Writing mode | 5 | Medium (2K-14K px) | Vertical metric accuracy |
| Letter spacing | 2 | Low (3K-5K px) | Kerning + spacing interaction |

## tspan Gap Analysis and Fix Plan {#tspan-gaps}

17 failing `e-tspan-*` tests (down from 21), categorized by feature.
Tests marked ✅ were fixed in this cycle.

### Category 1: Text chunk splitting (~~e-tspan-005~~, ~~e-tspan-006~~, e-tspan-031)

Mixed text and tspan content where text flows across element boundaries.

| Test | Pixels | Status | SVG Pattern |
|------|--------|--------|-------------|
| ~~005~~ | ~~4,385~~ | ✅ Fixed | `<tspan fill="green">Text</tspan> Text` — basic mixed content |
| ~~006~~ | ~~2,158~~ | ✅ Fixed | Nested `<tspan fill="green">Text <tspan font-weight="bold">Text</tspan></tspan>` |
| 031 | 388 | Triple-nested tspans with only whitespace between them |

**Root cause**: Text content before/after/between child tspans isn't being split into
separate spans correctly. The `textChunks` mechanism in `TextComponent` splits text at
child element boundaries, but `TextSystem::collectSpans` may not be reassembling them
in the right order with correct positioning.

### Category 2: Whitespace handling / xml:space (e-tspan-009, 010, 011)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 009 | 2,983 | Parent default, child `xml:space="preserve"` |
| 010 | 5,597 | Parent `xml:space="preserve"`, child `xml:space="default"` |
| 011 | 1,156 | Three levels: preserve → default → preserve |

**Root cause**: `xml:space` inheritance across tspan boundaries. The whitespace
normalization in `TextSystem` needs to respect per-element xml:space overrides during
inter-span space collapsing.

### Category 3: Positioning — dy on tspan (e-tspan-014)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 014 | 4,373 | Second sibling tspan has `dy="23"` |

**Root cause**: Relative positioning (`dy`) on tspan may not correctly accumulate
with the parent's pen position.

### Category 4: Per-character rotate (e-tspan-016, ~~e-tspan-017~~, e-tspan-029)

| Test | Pixels | Status | SVG Pattern |
|------|--------|--------|-------------|
| 016 | 1,764 | | Complex nested rotate lists across 4 levels of tspan nesting |
| ~~017~~ | ~~1,323~~ | ✅ Fixed | Simple `rotate="25"` on child tspan |
| 029 | 7,191 | | Rotate + `display:none` tspan — hidden chars consume rotate indices |

**Root cause**: Rotate list inheritance across tspan boundaries. The root `<text>`'s
rotate list should apply to all characters globally, with child tspan rotate lists
overriding for their character range. `display:none` tspans consume rotate indices
but don't render.

### Category 5: SVG 2 features on tspan (e-tspan-018, 020, 021, 022)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 018 | 2,051 | `opacity="0.5"` on tspan |
| 020 | 9,423 | `mask` on tspan |
| 021 | 4,442 | `clip-path` on tspan |
| 022 | 37,351 | `filter` on tspan |

**Root cause**: SVG 2 allows presentation attributes (opacity, mask, clip-path, filter)
on `<tspan>`. Requires treating tspan as a compositing layer. Not implemented —
these need per-tspan rendering isolation in the renderer.

### Category 6: Cross-tspan shaping (~~e-tspan-023~~, e-tspan-024)

| Test | Pixels | Status | SVG Pattern |
|------|--------|--------|-------------|
| ~~023~~ | ~~4,218~~ | ✅ Fixed | `T<tspan fill="green">e</tspan>xt` — color change mid-word |
| 024 | 16,722 | | `A<tspan font-weight="bold">V</tspan>A` — weight change + text-anchor |

**Root cause**: 023 was fixed by per-span font resolution (bold tspan now uses
NotoSans-Bold.ttf). 024 still fails because the bold "V" has different advance widths
than the reference, and `text-anchor="middle"` amplifies the positioning error.

### Category 7: Gradient bbox on tspan (e-tspan-025, 030)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 025 | 2,773 | `<tspan fill="url(#lg1)">` — gradient should use text bbox |
| 030 | 5,478 | Gradient + preserved spaces + text-decoration |

**Root cause**: When a tspan has `fill="url(#gradient)"`, the gradient's
`objectBoundingBox` should resolve against the parent `<text>` element's bbox,
not the tspan's own extent.

### Category 8: BIDI (e-tspan-026)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 026 | 2,919 | Mixed LTR/RTL with gradient tspan crossing script boundary |

**Root cause**: Bidirectional text reordering not implemented. Same root cause as
`e-text-035`.

### Category 9: Per-character coordinates on tspan (e-tspan-027)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 027 | 16,873 | `<tspan y="100 110 120 130">T<tspan y="50">ex</tspan></tspan>t` |

**Root cause**: Per-character y-coordinates on tspan with nested tspan overriding.
The child tspan's `y="50"` should override the parent's y-list for its characters.
Coordinate inheritance across tspan nesting not correctly implemented.

### Category 10: Font size changes (e-tspan-028)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| 028 | 12,001 | `<tspan>T<tspan font-size="80">ex</tspan></tspan>t` |

**Root cause**: When font-size changes within a nested tspan, the baseline must be
recalculated. Currently all glyphs in a span share the same font size. Needs per-span
font resolution in the shaping pipeline.

### Fix Plan (priority order)

#### Phase A: Text chunk splitting ✅ Done (fixes 005, 006, 017, 023)

Two fixes in `SVGTextContentElement` and `TextSystem`:

1. **`advanceTextChunk` chunk[0] guard**: When the XML parser strips whitespace-only text
   nodes before a child tspan, `textChunks` was empty when `advanceTextChunk` was called.
   Text after the child element ended up in chunk[0] instead of chunk[1], breaking DOM order.

2. **`pos.x[ci]` gating by `applyElementPositioning`**: For continuation text chunks (text
   after a child element), `pos.x[0]` was being re-applied to `xList[0]`, resetting the pen
   to the start position and overlapping with the tspan's text.

3. **`font-weight` CSS property + per-span font resolution**: Added `font-weight` as a
   presentation attribute in `PropertyRegistry` (parses "bold"→700, "normal"→400, numeric).
   `TextShaper::layout` now resolves font per-span using `FontManager::findFont(family, weight)`.

**Fixed tests**: e-tspan-005, e-tspan-006, e-tspan-017, e-tspan-023

#### Phase B: Whitespace and xml:space (fixes 009, 010, 011)

Fix inter-span whitespace collapsing to respect per-element `xml:space` overrides.
Currently `TextSystem` applies whitespace rules globally; needs per-span tracking.

**Tests**: e-tspan-009, e-tspan-010, e-tspan-011

#### Phase C: Positioning — dy on tspan (fixes 014)

Ensure `dy` on tspan correctly offsets from the current pen position. May already work
for the first character but fail for continuation text.

**Tests**: e-tspan-014

#### Phase D: Rotate inheritance (fixes 016, 029)

Implement global rotate list from root `<text>` that applies to all characters across
tspan boundaries. Child tspan rotate lists override for their character range.
Handle `display:none` consuming rotate indices without rendering.

**Tests**: e-tspan-016, e-tspan-029

#### Phase E: Cross-tspan shaping — bold weight (fixes 024)

The bold "V" in `A<tspan font-weight="bold">V</tspan>A` uses a different font (NotoSans-Bold)
which has different advance widths and kerning. Combined with `text-anchor="middle"`, the
positioning error is amplified. Needs cross-font kerning or text-anchor recalculation after
font changes.

**Tests**: e-tspan-024

#### Phase F: Per-character coordinates on nested tspan (fixes 027)

Implement coordinate list inheritance: child tspan's x/y/dx/dy lists override the
parent's lists for the child's character range. Remaining parent coordinates apply
to characters after the child.

**Tests**: e-tspan-027

#### Phase G: Mixed font-size (fixes 028)

Support per-span font resolution in the shaper. When font-size changes in a nested
tspan, re-resolve the font at the new size and shape that span's text separately
with correct baseline alignment.

**Tests**: e-tspan-028

#### Phase H: Gradient bbox (fixes 025, 030)

When resolving `objectBoundingBox` for gradients on tspan, use the parent `<text>`
element's computed bbox instead of the tspan's own extent.

**Tests**: e-tspan-025, e-tspan-030

#### Phase I: SVG 2 tspan features (fixes 018, 020, 021, 022)

Implement compositing layer for tspan: render tspan content to a temporary surface,
apply opacity/mask/clip-path/filter, then composite back. Requires renderer changes
to support per-tspan isolation.

**Tests**: e-tspan-018, e-tspan-020, e-tspan-021, e-tspan-022

#### Phase J: BIDI (fixes 026)

Implement bidirectional text reordering. Shared with `e-text-035`.

**Tests**: e-tspan-026
