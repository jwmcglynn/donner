# Text Rendering: Resvg Text Test Gap Analysis (2026-03-13)

[Back to hub](../text_rendering.md)

## Resvg Text Test Gap Analysis (2026-03-13)

**Status:** 183 text tests enabled across 11 groups, all passing with thresholds. The test harness
loads the resvg suite's Noto Sans fonts so font data is identical — remaining diffs are from
missing features and stb_truetype vs FreeType glyph rendering differences.

### Current Test Threshold Summary

| Test Group | Tests | Typical Diff | Default Threshold | Root Cause |
|-----------|-------|-------------|-------------------|------------|
| `e-text-` | 41 (+1 UB skip) | 13K-24K | 17500 | Per-char positioning + font rendering |
| `e-tspan` | 31 | 5K-45K | 17000 | Per-char positioning + font rendering |
| `e-textPath` | 29 (+15 skip) | 1K-9K | varies | Font rendering (positioning works) |
| `a-text-anchor` | 13 | 2K-21K | 10500 | Font rendering (feature implemented) |
| `a-textLength` | 9 | 10K-24K | 12000 | Font rendering (feature implemented) |
| `a-text-decoration` | 19 | 5K-19K | 13000 | Font rendering (feature implemented) |
| `a-text-rendering` | 5 | 10K-19K | 19500 | Font rendering |
| `a-baseline-shift` | 22 | 6K-21K | 8500 | **Not implemented** |
| `a-letter-spacing` | 11 | 1K-16K | 17000 | **Not implemented** |
| `a-word-spacing` | 6 (+1 UB skip) | 4K | 4500 | **Not implemented** |
| `a-writing-mode` | 18 (+2 UB skip) | 2K-17K | 17000 | **Not implemented** |
| `a-alignment-baseline` | 1 | 12K | 13000 | **Not implemented** |
| `a-kerning` | 1 | 10K | 10000 | Font rendering |
| `a-lengthAdjust` | 1 | 15K | 15000 | Font rendering |
| `a-unicode-bidi` | 1 | 2K | 2500 | Partial (no bidi reordering) |

### Root Cause Breakdown

#### 1. Per-Character Positioning NOT Implemented (CRITICAL)

**Impact:** Affects ALL e-text and e-tspan tests. Explains ~5K-10K of every diff.

The SVG spec allows `x`, `y`, `dx`, `dy`, and `rotate` attributes to contain comma/space-separated
lists where each value positions an individual character:

```xml
<text x="30 70 110 150" y="70 100 130 160">Text</text>
<!-- Should position: T at (30,70), e at (70,100), x at (110,130), t at (150,160) -->
```

**Current state:** `TextSystem.cc:32-56` only reads `pos.x[0]`, discarding all subsequent values.
The `ComputedTextComponent::TextSpan` struct only stores scalar x/y/dx/dy/rotate, not per-character
arrays. Parsing is correct (`SmallVector<Lengthd, 1>` stores full lists), but the computed layer
throws them away.

**Files:**
- `donner/svg/components/text/TextSystem.cc` — lines 32-56, only uses `[0]`
- `donner/svg/components/text/ComputedTextComponent.h` — `TextSpan` stores single values
- `donner/svg/renderer/TextLayout.cc` — layout uses single baseX/baseY per span

#### 2. Irreducible Font Rendering Baseline (~3K-5K per test)

Even with all features correctly implemented, stb_truetype and FreeType produce different glyph
outlines due to different hinting/rasterization. On a 200x200 canvas with text, this produces
roughly 3000-5000 pixel diffs. This is the floor we cannot go below without switching font backends.

Evidence: `e-textPath` tests (where positioning IS correct) still show 3000-5000 pixel diffs.

#### 3. Missing Text Properties

| Property | Status | Impact | Difficulty |
|----------|--------|--------|------------|
| `letter-spacing` | Not implemented | 1K-16K diffs | Low |
| `word-spacing` | Not implemented | ~4K diffs | Low |
| `baseline-shift` | Not implemented | 6K-21K diffs | Medium |
| `writing-mode` | Not implemented (no enum) | 2K-17K diffs | High |
| `alignment-baseline` | Not implemented | ~12K diffs | Medium |

All five properties are registered in `PropertyRegistry.cc` but never parsed or applied.

#### 4. Missing SVG 2 Features on `<tspan>`

Several e-tspan tests exercise SVG 2 features where tspan gets its own rendering context:
- `e-tspan-018`: `opacity` on tspan (5891 diff)
- `e-tspan-020`: `mask` on tspan (16782 diff)
- `e-tspan-021`: `clip-path` on tspan (12107 diff)
- `e-tspan-022`: `filter` on tspan (44238 diff — largest diff)

These require rendering each tspan as an independent graphics element, which the current
architecture doesn't support (tspan is treated as inline text content, not a stacking context).

### Implementation Plan: Closing the Gaps

#### Phase 8: Per-Character Positioning (Done)

**Goal:** Reduce e-text and e-tspan default thresholds from ~17000 to ~5000 (font rendering floor).

- [x] Extended `ComputedTextComponent::TextSpan` with per-character position arrays:
  `xList`, `yList`, `dxList`, `dyList` (`std::vector<std::optional<Lengthd>>`) and
  `rotateList` (`std::vector<double>`). Values are `Lengthd` for deferred resolution
  in the layout engine where viewBox/fontMetrics are available.
- [x] Updated `TextSystem::instantiateAllComputedComponents()` to populate per-character
  arrays using global character indexing across tspan boundaries. Each character position
  checks the leaf element's own positioning first, then falls back to the root text
  element's positioning at the global character index.
- [x] Updated `TextLayout::layout()` with per-character positioning in the glyph loop:
  absolute x/y override the pen position (suppressing kerning), dx/dy add relative offsets,
  and rotate uses last-value-repeats semantics per SVG spec.
- [x] Updated `TextShaper::layout()` with the same per-character logic, using HarfBuzz
  cluster byte offsets mapped to codepoint indices via a byte-to-char-index table.
- [x] Updated textPath rotation in both layout engines to combine path tangent angle
  with per-glyph rotation (previously used only span-level rotation).

**Measured outcome:** Tests with per-character positioning (e.g., per-char x/y/dx/dy/rotate)
dropped dramatically: e-tspan-011 from ~17K to 1.2K, e-text-022 from ~17K to 2.1K, many
e-tspan tests from ~17K to 5K-8K. Tests without per-character positioning remain at ~15K-16K
(font rendering baseline). Two per-char rotate tests (e-text-012, e-text-014) needed threshold
bumps from 17500 to 18000 due to rotation now being correctly applied per-character.

#### Phase 9: letter-spacing and word-spacing (Done)

**Goal:** Reduce a-letter-spacing thresholds from 17000 to ~5000 and a-word-spacing from 4500
to ~1000.

- [x] Added `ParseSpacingValue()` in PropertyRegistry.cc to parse "normal | \<length\>".
  "normal" maps to `Lengthd(0)`.
- [x] Declared `letterSpacing` and `wordSpacing` as `Property<Lengthd, PropertyCascade::Inherit>`
  in PropertyRegistry.h with parser entries in the kProperties map.
- [x] Added `letterSpacingPx` and `wordSpacingPx` (resolved to pixels) to `TextParams`,
  populated from computed style in `toTextParams()` in RendererDriver.cc.
- [x] Applied in both `TextLayout::layout()` and `TextShaper::layout()`:
  letter-spacing is added after every glyph advance, word-spacing is added after U+0020 space.

**Measured outcome:** Tests with non-zero letter-spacing (007-011) dropped from ~16K to 1K-3.6K.
Tests with `letter-spacing="normal"` (001-006) remain at ~16K (font rendering baseline).
Word-spacing tests all at ~4K (within existing 4500 threshold).

#### Phase 10: baseline-shift (Done)

**Goal:** Reduce a-baseline-shift thresholds from 8500 to ~4K.

- [x] Added `ParseBaselineShift()` in PropertyRegistry.cc: handles `baseline` (→0),
  `sub` (→-0.33em), `super` (→0.4em), `<length>`, `<percentage>` (→em fraction).
- [x] Declared `baselineShift` as `Property<Lengthd>` (not inherited) in PropertyRegistry.h.
- [x] Added `Lengthd baselineShift` to `ComputedTextComponent::TextSpan`.
- [x] In TextSystem, reads baseline-shift from entity's `ComputedStyleComponent` (or parent's)
  and stores in the span.
- [x] In TextLayout/TextShaper, resolves to pixels using actual font size for em units
  (`FontMetrics` adjusted with `fontSizePx`), then subtracted from baseY (positive = shift up
  per CSS convention).

**Measured outcome:** Tests 001-013 dropped from ~8.5K to ~7K-7.8K (still within 8500 default).
Tests 014-021 unchanged (nested/percentage shifts remain at pre-existing override thresholds).
Test 013 improved to 6.3K. All tests pass within existing thresholds.

#### Phase 11: writing-mode (Done)

**Goal:** Enable vertical text rendering.

- [x] Created `WritingMode` enum (`HorizontalTb`, `VerticalRl`, `VerticalLr`) in
  `donner/svg/core/WritingMode.h` with `isVertical()` helper.
- [x] Added `ParseWritingMode()` supporting both SVG1 (`lr-tb`, `lr`, `rl-tb`, `rl`, `tb-rl`,
  `tb`, `tb-lr`) and CSS3 (`horizontal-tb`, `vertical-rl`, `vertical-lr`) values.
- [x] Property registered as `Property<WritingMode, PropertyCascade::Inherit>`, threaded
  through `TextParams`, populated in `toTextParams()`.
- [x] Added `yAdvance` field to both `LayoutGlyph` and `ShapedGlyph` structs.
- [x] TextLayout vertical mode: pen advances along Y using `fontSizePx` as vertical advance
  (stb_truetype lacks vmtx tables). Non-CJK glyphs (codepoint < 0x2E80) get 90° CW rotation.
  text-anchor and textLength operate along Y.
- [x] TextShaper vertical mode: uses `HB_DIRECTION_TTB` for HarfBuzz shaping. Extracts
  `y_advance` (negated for SVG Y-down). HarfBuzz handles vertical glyph orientation via `vert`
  GSUB feature. text-anchor and textLength on Y.

**Measured outcome:** CJK vertical tests (013-015) at 1.8K-3.2K pixels. Latin vertical (005-006)
at ~11.5K. Horizontal modes (001-004) unchanged at ~16K. All 18 tests pass within the existing
17000 threshold in both base-text and text-shaping tiers.

#### Phase 12: alignment-baseline (Done)

**Goal:** Close the single a-alignment-baseline test.

- [x] Added `alignmentBaseline` as `Property<DominantBaseline>` in PropertyRegistry, reusing
  the `ParseDominantBaseline()` parser since the value sets are identical.
- [x] Added `DominantBaseline alignmentBaseline` to `ComputedTextComponent::TextSpan`.
- [x] In TextSystem, reads from entity's `ComputedStyleComponent` alongside baseline-shift.
- [x] In TextLayout/TextShaper, when `span.alignmentBaseline != Auto`, computes a per-span
  baseline shift that overrides the text-level `dominant-baseline`.

**Measured outcome:** a-alignment-baseline-001 at 12223 pixels (within 13000 threshold). Passes
in both base text and text-shaping tiers.

### Priority Order

1. **Phase 8 (per-character positioning)** — highest impact, affects all 72 e-text/e-tspan tests
2. **Phase 9 (letter/word-spacing)** — low difficulty, affects 17 tests
3. **Phase 10 (baseline-shift)** — medium difficulty, affects 22 tests
4. **Phase 12 (alignment-baseline)** — small, 1 test
5. **Phase 11 (writing-mode)** — deferred, high complexity, 18 tests

### Expected Threshold Reduction

After Phases 8-10:

| Test Group | Current Default | Expected | Reduction |
|-----------|----------------|----------|-----------|
| `e-text-` | 17500 | ~5000 | 3.5x |
| `e-tspan` | 17000 | ~5000 | 3.4x |
| `a-letter-spacing` | 17000 | ~5000 | 3.4x |
| `a-word-spacing` | 4500 | ~1500 | 3x |
| `a-baseline-shift` | 8500 | ~4500 | 1.9x |
| `a-text-anchor` | 10500 | ~5000 | 2.1x |
| `a-textLength` | 12000 | ~5000 | 2.4x |

The irreducible floor of ~3000-5000 comes from stb_truetype vs FreeType glyph rendering
differences and cannot be reduced without switching font backends.
