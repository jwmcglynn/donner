# Text Rendering: Testing and Validation

[Back to hub](../text_rendering.md)

## Testing and Validation

### Unit tests

- `FontManager` tests: load TTF/OTF/WOFF1, verify `stbtt_fontinfo` initialization, test
  cascade fallback, verify font metrics.
- `TextEngine` helper tests: verify chunking, text-anchor, `textLength`, `lengthAdjust`,
  and baseline-shift calculations without real fonts.
- `TextBackend` tests: verify shaping, metrics, and outline extraction for both base and full
  backends.
- Glyph outline tests: extract outlines for known glyphs, verify path geometry matches expected
  curves.

### Golden image tests

- Add text-specific SVG test cases to `renderer_tests`:
  - Basic Latin text.
  - Multi-span positioning (dx/dy).
  - `text-anchor` (start/middle/end).
  - `@font-face` with embedded WOFF1 font.
  - `@font-face` with WOFF2 font (requires `text_full`).
  - Stroke + fill text.
  - Per-glyph rotation.
- Resvg text test subset: most `e-text-*` and `e-tspan-*` tests now pass with tight
  thresholds. Primary remaining failures are in `e-textPath-*` and `a-font-size-*`.

### Feature-gated test skipping

```cpp
{"text_basic.svg", Params::RequiresFeature(Feature::Text)},
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextFull)},
```

CI runs all feature combinations to ensure both enabled and disabled paths work.

### Backend parity

- Text golden images differ between Skia and TinySkia at the base tier because Skia uses
  its own internal layout (with GPOS access) while TinySkia uses stb_truetype (kern-table only).
- At the `text_full` tier, shaping output is identical between backends since both use
  HarfBuzz, and golden images should be much closer.
- TinySkia renders glyph outlines as vector paths (no hinting), so small-size text will look
  slightly different from Skia's hinted rasterization regardless of shaping tier.

## Current Snapshot (2026-04-04) {#current-snapshot}

Current base-tier (`text`, TinySkia) resvg status for the most relevant text slices:

| Slice | Current status | Notes |
|---|---|---|
| `e-text-*` | 30/30 enabled tests passing | 12 disabled: mostly text-full-only or explicit skips |
| `e-tspan-*` | 23/24 enabled tests passing | Active failure: `e-tspan-030`; 7 disabled |
| `a-text-decoration-*` | All enabled tests passing | Includes custom golden for `019` |
| `a-lengthAdjust-*` | All enabled tests passing | `spacingAndGlyphs` now stretches glyphs along text direction |
| `a-dominant-baseline-*` | Enabled coverage passing at default threshold | Coverage is still thin |
| `a-letter-spacing-*` | All enabled tests passing in base tier | Arabic case requires text-full; 3 disabled/UB cases remain |
| `a-textLength-*` | All enabled tests passing | `a-textLength-008` still disabled |
| `a-writing-mode-*` | All enabled tests passing | 10 disabled mixed-script / rotate / dx/dy cases remain |
| `e-textPath-*` | Still disabled | Largest intentional feature gap |

Release-significant gaps from the current snapshot:

- `e-tspan-030` is the main remaining active base-tier failure.
- `e-textPath-*` is still intentionally out of the enabled suite.
- BiDi and advanced mixed-script vertical text remain explicitly deferred.

## Historical Failure Analysis (2026-03-30) {#test-failures}

Historical snapshot of resvg test suite failures for the base config (TinySkia backend).
73 tests fail at the default threshold (100 max pixels).

Note: Previous counts (42) were artificially low because the `a-font` suite used a
17,000px default threshold that masked failures. The true count with the standard
100px threshold is 73.

### Failure Summary

| Category | Failing | Disabled | Pixel Range | Severity |
|----------|---------|----------|-------------|----------|
| textPath | 34 | 8 | 112--13,209 | Medium-High |
| Font properties | 31 | 2 | 990--6,265 | High |
| Writing mode | 3 | 3 | 3,286--14,562 | Medium |
| tspan | 3 | 6 | 2,159--6,324 | Medium |
| Letter spacing | 1 | 0 | 3,055 | Low |
| Text length | 1 | 0 | 13,790 | Medium |

### Font size rendering -- RESOLVED {#font-size-summary}

All 8 previously-failing font-size tests now pass. The 2 previously-disabled tests
(004, 020) are now enabled and passing. See [Font size gap analysis](#font-size-gaps)
for details.

**What was fixed (2026-03-30):** Font-size is now resolved to absolute pixels during
the CSS cascade (`StyleSystem::computePropertiesInto`), using the parent's computed
font-size for relative units. Previously, em/ex/% units resolved against a hardcoded
16px default. Additionally, `larger`/`smaller` keywords are now parsed (converted to
120%/83.3% of parent).

Previously all 8 tests involved CSS `font-size` with relative units (em, ex, %, `larger`/`smaller`
keywords) that must resolve against the parent element's computed font-size.

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| a-font-size-003 | 34,397 | `font-size="5ex"` with parent `font-size="12"` |
| a-font-size-006 | 34,308 | `font-size="larger"` then `font-size="80%"` chain |
| a-font-size-007 | 39,492 | `font-size="300%"` (no explicit parent) |
| a-font-size-012 | 23,675 | parent `font-size="0"`, child `font-size="50%"` |
| a-font-size-014 | 26,994 | `font-size="3em"` with parent `font-size="12"` |
| a-font-size-015 | 18,603 | `font-size="200%"` with parent `font-size="20"` |
| a-font-size-016 | 26,994 | `font-size="3em"` on root `<svg>` element |
| a-font-size-017 | 18,603 | `font-size="5ex"` on root `<svg>` element |

See [Font size gap analysis](#font-size-gaps) for root cause details and fix plan.

### Font properties (31 failures) {#font-properties-summary}

31 `a-font-*` tests fail at the standard 100px threshold. These were previously
hidden by a 17,000px default max-mismatched-pixels override on the entire `a-font`
prefix. Grouped by unimplemented feature:

| Sub-category | Failing | Pixel Range | Implementation Status |
|--------------|---------|-------------|----------------------|
| Generic font-family mapping | 10 | 990--5,375 | Not implemented |
| font-weight (bold variants) | 7 | 2,719--5,470 | Partial |
| font-style | 3 | 2,375 | Not implemented |
| font-stretch | 3 | 3,950 | Not implemented |
| font-variant | 2 | 5,570 | Not implemented |
| font-size keywords | 3 | 1,010--4,699 | Not implemented |
| Residual rendering diffs | 3 | 4,134--6,265 | Rendering engine difference |

See [Font property gap analysis](#font-property-gaps) for details.

### textPath rendering (34 failures) {#textpath-summary}

All `e-textPath-*` tests are commented out in `resvg_test_suite.cc` (lines 999--1082)
and run with the default 100px threshold, so all 34 fail. Tests 006 and 016 pass
(startOffset overflow and link-to-rect respectively).

| Severity | Tests | Pixel Range |
|----------|-------|-------------|
| Near-passing (<500px) | 034, 026, 005, 020 | 112--396 |
| Low (500--2,000px) | 001, 014, 037, 029, 028, 039, 002, 003, 004, 015, 009, 038 | 626--1,651 |
| Medium (2,000--5,000px) | 011, 022, 024, 017, 018, 019, 023, 031, 021, 012, 010, 025 | 2,113--4,900 |
| High (>5,000px) | 013, 030, 027, 036, 035, 032 | 5,242--13,209 |

See [textPath gap analysis](#textpath-gaps) for categorization and fix plan.

### Writing mode (3 failures)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| a-writing-mode-010 | 14,562 | `writing-mode="tb"` + `text-anchor="middle"`, font-size 64 |
| a-writing-mode-013 | 3,286 | `writing-mode="tb"` with mixed scripts (Japanese + English + Arabic) |
| a-writing-mode-020 | 10,599 | `writing-mode="tb"` with `dy="-105"` on second tspan |

See [writing mode gap analysis](#writing-mode-gaps) for root cause details.

### tspan positioning (3 active failures + 6 disabled)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| e-tspan-006 | 2,159 | Nested `<tspan fill="green">Text <tspan font-weight="bold">Text</tspan></tspan>` |
| e-tspan-024 | 3,599 | `A<tspan font-weight="bold">V</tspan>A` + `text-anchor="middle"` |
| e-tspan-028 | 6,324 | `<tspan>T<tspan font-size="80">ex</tspan></tspan>t` |

Disabled: e-tspan-011 (xml:space preserve whitespace-only nodes), 016 (nested rotate),
020/021/022 (SVG 2 mask/clip-path/filter), 026 (BIDI + gradient).

See [tspan gap analysis](#tspan-gaps) for phase status.

### Letter spacing (1 failure)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| a-letter-spacing-009 | 3,055 | `letter-spacing="5"` with Arabic script + `text-anchor="middle"` |

Root cause: letter-spacing interaction with RTL/Arabic shaping and text-anchor centering.
Requires BIDI support to fix properly.

### Text length (1 failure)

| Test | Pixels | SVG Pattern |
|------|--------|-------------|
| a-textLength-008 | 13,790 | `textLength` on both `<text>` (500) and `<tspan>` (50, 120) elements |

Root cause: `textLength` glyph adjustment when specified on both parent and child elements.
The parent's `textLength` should distribute remaining space after child tspans have applied
their own adjustments.

### Changes since 2026-03-29

**10 font-size tests newly passing (8 fixed + 2 enabled):**

| Test | Before | After | What was fixed |
|------|--------|-------|----------------|
| a-font-size-003 | 34,397 | 0 | Font-size cascade: ex + parent resolution |
| a-font-size-004 | Skipped | 0 | Enabled: nested percentage now works |
| a-font-size-006 | 34,308 | 0 | `larger`/`smaller` keyword parsing |
| a-font-size-007 | 39,492 | 0 | Font-size cascade + golden override (resvg wrong) |
| a-font-size-012 | 23,675 | 0 | Font-size cascade: % of zero parent |
| a-font-size-014 | 26,994 | 0 | Font-size cascade: em + parent resolution |
| a-font-size-015 | 18,603 | 0 | Font-size cascade: % + parent resolution |
| a-font-size-016 | 26,994 | 0 | Font-size cascade + golden override (resvg wrong) |
| a-font-size-017 | 18,603 | 0 | Font-size cascade + golden override (resvg wrong) |
| a-font-size-020 | Skipped | 0 | Enabled: nested percentage now works |

### Changes since 2026-03-21

**7 tspan tests newly passing:**

| Test | Before | After | What was fixed |
|------|--------|-------|----------------|
| e-tspan-009 | 2,983 | 21 | xml:space preserve-to-default boundary handling |
| e-tspan-010 | 5,597 | 6 | xml:space default-to-preserve boundary handling |
| e-tspan-014 | 4,373 | 2 | dy positioning on sibling tspan |
| e-tspan-025 | 2,773 | 10 | Gradient fill on tspan (objectBoundingBox) |
| e-tspan-027 | 16,873 | 0 | Per-character y-coordinates on nested tspan |
| e-tspan-029 | 7,191 | 0 | Rotate + display:none tspan index consumption |
| e-tspan-030 | 5,478 | 379 | Gradient + preserved spaces + text-decoration |

**2 tspan tests improved but still failing:**

| Test | Before | After | Improvement |
|------|--------|-------|-------------|
| e-tspan-024 | 16,722 | 3,599 | 78% reduction (cross-tspan bold shaping) |
| e-tspan-028 | 12,001 | 6,324 | 47% reduction (mixed font-size baseline) |

## tspan Gap Analysis and Fix Plan {#tspan-gaps}

3 actively failing + 6 disabled `e-tspan-*` tests.
Phases B, C, D (partial), F, H completed since 2026-03-21.

### Phase status

| Phase | Description | Status | Tests |
|-------|-------------|--------|-------|
| A | Text chunk splitting | Partial (006 still fails) | ~~005~~, 006, ~~031~~ |
| B | Whitespace/xml:space | **COMPLETE** | ~~009~~, ~~010~~, 011 (disabled) |
| C | dy positioning | **COMPLETE** | ~~014~~ |
| D | Rotate inheritance | Partial (029 fixed, 016 disabled) | ~~017~~, ~~029~~, 016 (disabled) |
| E | Cross-tspan bold | Improved (16,722 to 3,599) | 024 |
| F | Per-char coordinates | **COMPLETE** | ~~027~~ |
| G | Mixed font-size | Improved (12,001 to 6,324) | 028 |
| H | Gradient bbox | **COMPLETE** | ~~025~~, ~~030~~ |
| I | SVG 2 features | Not started | 020, 021, 022 (disabled) |
| J | BIDI | Not started | 026 (disabled) |

### Phase A: Text chunk splitting -- Partial

Fixed tests: e-tspan-005 (basic mixed content), e-tspan-031 (triple-nested whitespace).

**Still failing**: e-tspan-006 (2,159px). Nested `<tspan fill="green">Text <tspan font-weight="bold">Text</tspan></tspan>` -- deeply nested tspan with fill inheritance and font-weight change. The inner content positioning is off when style changes cascade through multiple nesting levels.

### Phase B: Whitespace and xml:space -- COMPLETE

Fixed: e-tspan-009 (preserve-to-default boundary), e-tspan-010 (default-to-preserve boundary).

Remaining disabled: e-tspan-011 (three-level preserve/default/preserve nesting). The XML
parser strips whitespace-only text nodes before a child tspan in default mode, losing
content that should be preserved when the child switches back to preserve mode.

### Phase C: dy positioning -- COMPLETE

Fixed: e-tspan-014 (dy on sibling tspan).

### Phase D: Rotate inheritance -- Partial

Fixed: e-tspan-017 (simple rotate on child tspan), e-tspan-029 (rotate + display:none
index consumption).

Remaining disabled: e-tspan-016 (1,764px). Complex nested rotate lists across 4 levels
of tspan nesting with partial overrides. The root text's rotate list should apply globally,
with child rotate lists overriding for their character range.

### Phase E: Cross-tspan bold (e-tspan-024, 3,599px)

`A<tspan font-weight="bold">V</tspan>A` with `text-anchor="middle"`. The bold "V" uses
NotoSans-Bold.ttf which has different advance widths, and `text-anchor="middle"` amplifies
the positioning error. Improved from 16,722px after per-span font resolution was added.

Remaining gap: Cross-font kerning between the regular "A" and bold "V" is not applied.
The total text width differs from the reference, causing the centered position to shift.

### Phase F: Per-character coordinates -- COMPLETE

Fixed: e-tspan-027 (per-character y-coordinates with nested tspan override).

### Phase G: Mixed font-size (e-tspan-028, 6,324px)

`<tspan>T<tspan font-size="80">ex</tspan></tspan>t` where font-size changes from 48 to 80
mid-text. Improved from 12,001px after per-span font resolution. Remaining gap: baseline
alignment when font-size changes -- the dominant baseline of the larger font should align
with the parent's baseline.

### Phase H: Gradient bbox -- COMPLETE

Fixed: e-tspan-025 (gradient fill objectBoundingBox), e-tspan-030 (gradient + preserved
spaces + text-decoration).

### Phase I: SVG 2 tspan features (disabled: 020, 021, 022)

SVG 2 allows `mask`, `clip-path`, and `filter` on `<tspan>`. Requires rendering each
tspan as a compositing layer with isolation. Not implemented.

### Phase J: BIDI (disabled: 026)

Bidirectional text reordering with gradient tspan crossing script boundary. Shared root
cause with `e-text-035`. Requires UAX#9 bidi algorithm implementation.

## textPath Gap Analysis {#textpath-gaps}

34 tests fail when run with the default 100px threshold. All overrides in
`resvg_test_suite.cc` lines 999--1082 are commented out. 8 additional tests are
disabled with `Params::Skip()`.

### Passing tests (2)

| Test | Feature | Why it passes |
|------|---------|---------------|
| e-textPath-006 | startOffset=9999 | Text is entirely past path end, nothing renders |
| e-textPath-016 | Link to `<rect>` (SVG 2) | Falls back to inline rendering |

### Category 1: Core glyph positioning (001--005)

Basic text-on-path with various startOffset values. These are the foundation for all
other textPath tests.

| Test | Pixels | Feature |
|------|--------|---------|
| 001 | 774 | Basic cubic Bezier path |
| 002 | 1,086 | startOffset=30 (absolute) |
| 003 | 993 | startOffset=5mm (length unit) |
| 004 | 985 | startOffset=10% (percentage) |
| 005 | 284 | startOffset=-100 (negative) |

**Root cause**: Arc-length parameterization accuracy. The `Path::parameterForArcLength`
binary search may have insufficient precision for cubic Bezier segments, leading to
systematic glyph placement offsets. The consistent 774--1,086px range (except 005 at 284)
suggests a small but consistent positioning error.

**Fix**: Increase precision of arc-length computation or switch to Gauss-Legendre
quadrature for cubic Bezier arc length. Test 005 is near-passing and may be fixed by
the same change.

### Category 2: Path geometry variants (009, 020, 024, 026, 027, 034, 037--039)

Different path geometries exercise edge cases in path walking.

| Test | Pixels | Feature |
|------|--------|---------|
| 009 | 1,506 | Two separate textPath elements in one text |
| 020 | 396 | Closed path (arc with Z) |
| 024 | 3,763 | Path with subpaths (M L M L) |
| 026 | 237 | Path with ClosePath (triangle, M L L Z) |
| 027 | 6,608 | Simple polygon (M L Z) |
| 034 | 112 | Arc command (M A) |
| 037 | 716 | Transform on group outside referenced path |
| 038 | 1,651 | Big letter-spacing on arc path |
| 039 | 954 | Subpaths + startOffset |

**Root cause**: Mixed issues. Tests 020, 026, 034 are near-passing (<500px), suggesting
closed paths and arcs mostly work. Test 027 (6,608px) has a simple M L Z path where the
ClosePath segment may not be handled correctly. Test 024 (3,763px) involves subpath
handling -- text should follow only the first subpath unless startOffset pushes past it.

**Fix priority**: Medium. Near-passing tests (020, 026, 034) may be fixed by Category 1
improvements. Tests 024, 027 need specific ClosePath/subpath logic.

### Category 3: Text positioning on path (013--015, 022--023)

Per-character x/y/dx/dy coordinates when text is on a path.

| Test | Pixels | Feature |
|------|--------|---------|
| 013 | 5,312 | x/y coordinates on parent `<text>` element |
| 014 | 770 | x/y coordinates on `<textPath>` element |
| 015 | 1,455 | Very long text (overflow past path end) |
| 022 | 3,005 | tspan with absolute x/y position inside textPath |
| 023 | 4,209 | tspan with relative dx/dy inside textPath |

**Root cause**: Absolute x/y positioning should map to distance-along-path rather than
canvas coordinates. Relative dx/dy should offset from current path position. Test 015
needs overflow handling (hide glyphs past path end).

**Fix priority**: Medium-High. Affects text with tspan children on paths.

### Category 4: Text properties on path (010, 019, 021, 028--032)

Various text properties that need special handling on text-on-path.

| Test | Pixels | Feature |
|------|--------|---------|
| 010 | 4,624 | Nested textPath elements |
| 019 | 4,183 | text-anchor (middle/end) |
| 021 | 4,505 | writing-mode=tb (vertical Japanese) |
| 028 | 827 | text-decoration (underline) |
| 029 | 626 | rotate attribute on text |
| 030 | 5,242 | Complex combination (writing-mode + letter-spacing + baseline-shift + startOffset) |
| 031 | 4,353 | letter-spacing |
| 032 | 13,209 | baseline-shift |

**Root cause**: Each property interacts with path layout differently:
- `text-anchor` must center/end-align along path length, not canvas X.
- `baseline-shift` offsets perpendicular to the path tangent.
- `text-decoration` (underline) should follow the path curve.
- `writing-mode=tb` changes the glyph orientation paradigm on path.
- `letter-spacing` adds to distance-along-path between glyphs.

**Fix priority**: Low-Medium. These are property interactions on top of core positioning.

### Category 5: Mixed content (011, 012, 025)

Text elements with both textPath and tspan children.

| Test | Pixels | Feature |
|------|--------|---------|
| 011 | 2,113 | Text before + textPath + tspan after |
| 012 | 4,524 | Multiple textPath + tspan interspersed |
| 025 | 4,900 | Invalid textPath element mid-content |

**Root cause**: When `<text>` has both `<textPath>` and `<tspan>` children, text after
the textPath should continue at the end position of the path or revert to normal
positioning.

**Fix priority**: Low.

### Category 6: Edge cases and transforms (017, 018, 035, 036)

| Test | Pixels | Feature |
|------|--------|---------|
| 017 | 4,025 | No href on textPath (should render as inline text) |
| 018 | 4,044 | Invalid href (points to `<text>`, should render inline) |
| 035 | 8,292 | `dy` with tiny coordinates + scale(100) transform |
| 036 | 7,334 | Transform attribute on referenced `<path>` element |

**Root cause**: Tests 017 and 018 test fallback behavior when textPath has no valid path
reference. Tests 035 and 036 involve transform application -- the referenced path's
transform must be applied to the path geometry before glyph placement.

**Fix priority**: Low.

### Disabled tests (8)

| Test | Reason |
|------|--------|
| 007 | Not implemented: `method="stretch"` |
| 008 | Not implemented: `spacing="auto"` |
| 033 | UB: baseline-shift + rotate combination |
| 040 | Not implemented: filter on textPath |
| 041 | Not implemented: `side="right"` (SVG 2) |
| 042 | Not implemented: `path` attribute (SVG 2) |
| 043 | Not implemented: `path` + `xlink:href` (SVG 2) |
| 044 | Not implemented: invalid `path` + `href` (SVG 2) |

### textPath fix plan

**Phase 1 -- Re-enable tests**: Uncomment the 34 tests in `resvg_test_suite.cc` with
per-test thresholds matching current pixel diffs. This prevents regressions.

**Phase 2 -- Core positioning** (fixes ~5 tests from Category 1): Improve arc-length
parameterization precision in `Path`. Target: tests 001--005 under threshold.

**Phase 3 -- Path geometry** (fixes ~5 tests from Category 2): Handle ClosePath
segments correctly (Z contributes path length back to start). Handle subpaths (text
follows first subpath only). Target: tests 020, 026, 027, 034.

**Phase 4 -- Positioning on path** (fixes ~5 tests from Category 3): Map absolute
x/y to distance-along-path. Map dx/dy to path offset. Add overflow handling (hide
past path end). Target: tests 013--015, 022--023.

**Phase 5 -- Properties on path** (fixes ~8 tests from Category 4): Implement
text-anchor along path length, baseline-shift perpendicular to tangent,
text-decoration following path curve. Target: tests 019, 028--032.

**Phase 6 -- Mixed content + edge cases** (fixes ~7 tests from Categories 5--6):
Text continuation after textPath, fallback for invalid refs, transform application.
Target: tests 011, 012, 017, 018, 025, 035, 036.

## Font Size Gap Analysis -- RESOLVED {#font-size-gaps}

**Status**: All 8 failures resolved (2026-03-30). 2 previously-disabled tests (004, 020)
also enabled and passing.

### What was fixed

Two changes in a single commit:

1. **Font-size cascade resolution** (`StyleSystem::computePropertiesInto`): After the
   CSS cascade, font-size is now resolved to absolute pixels using the parent's computed
   font-size. This fixes em, ex, and percentage units. A dedicated
   `PropertyRegistry::resolveFontSize()` method handles the font-size-specific percentage
   semantics (% of parent font-size, not viewBox).

2. **`larger`/`smaller` keyword parsing** (`PropertyRegistry.cc` font-size parser):
   These relative keywords are now parsed and converted to 120% / 83.3% respectively,
   which the cascade resolution then computes correctly.

### Results

| Test | Before | After | Root Cause Fixed |
|------|--------|-------|------------------|
| 003 | 34,397 | 0 | ex + parent font-size |
| 004 | Skipped | 0 | Nested percentage |
| 006 | 34,308 | 0 | `larger` keyword + chain |
| 007 | 39,492 | 0 | % resolved against viewBox; resvg golden overridden |
| 012 | 23,675 | 0 | % of zero |
| 014 | 26,994 | 0 | em + parent font-size |
| 015 | 18,603 | 0 | % + parent font-size |
| 016 | 26,994 | 0 | em on root element; resvg golden overridden |
| 017 | 18,603 | 0 | ex on root element; resvg golden overridden |
| 020 | Skipped | 0 | Nested percentage |

Tests 007, 016, 017 use custom golden overrides (`WithGoldenOverride`) — the resvg
goldens are incorrect (confirmed via Chrome). Our font-size resolution is correct:
300% of 16px = 48px, 3em * 16px = 48px, 5ex * 0.5 * 16px = 40px.

### Previous root cause analysis (for reference)

**Relative unit resolution**: Font-size values like `3em`, `200%`, `5ex`, and
`larger` are relative to the parent element's computed font-size. The previous
implementation resolved these against a hardcoded 16px default instead of the
actual parent's computed value, and percentages resolved against the viewBox
diagonal instead of the parent font-size.

### Future improvement: ex unit accuracy

The `exUnitInEm` ratio is still hardcoded at 0.5 (the CSS fallback). Measuring the
actual font's x-height from the OS/2 `sxHeight` field or the 'x' glyph bounding box
would improve accuracy for ex-unit tests, but the remaining diffs are small enough to
be within threshold.

## Font Property Gap Analysis {#font-property-gaps}

31 `a-font-*` tests fail. The core issue is that only `font-family` (lookup by name),
`font-size`, and `font-weight` (numeric/bold/normal) are fully wired through to
rendering. Several font properties are either not parsed, not used during font
selection, or missing key feature support.

### Implementation status

| Property | Parsed | Stored | Used in rendering | Missing |
|----------|--------|--------|-------------------|---------|
| `font-family` | Yes | `SmallVector<RcString>` | Yes (name lookup) | Generic family mapping |
| `font-size` | Yes | `Lengthd` | Yes | Absolute-size keywords |
| `font-weight` | Yes | `int` (100--900) | Yes (TextShaper only) | `bolder`/`lighter` keywords |
| `font-style` | No | Unparsed | No | Entire property |
| `font-stretch` | No | Unparsed | No | Entire property |
| `font-variant` | No | Unparsed | No | Entire property |

### 1. Generic font-family mapping -- NOT IMPLEMENTED (10 tests)

Generic families (`serif`, `sans-serif`, `monospace`, `cursive`, `fantasy`) are parsed
and stored as literal strings but `FontManager::findFont()` has no mapping from generic
names to actual fonts. All generic families render with the fallback font (Public Sans).

| Test | Pixels | What's tested |
|------|--------|---------------|
| family-001 | 3,854 | `serif` |
| family-002 | 2,123 | `sans-serif` |
| family-003 | 2,727 | `cursive` |
| family-004 | 5,375 | `fantasy` |
| family-005 | 3,924 | `monospace` |
| family-007 | 1,271 | Named font `Source Sans Pro` (not in `@font-face`) |
| family-008 | 1,271 | Font list: `'Source Sans Pro', Noto Sans, serif` |
| family-009 | 5,228 | Fallback from `Invalid` family |
| family-010 | 990 | Fallback: `Invalid, Noto Sans` |
| family-011 | 3,729 | Bold `sans-serif` |

**What's needed**: Add a generic-family-to-font mapping in `FontManager`. The mapping
could be configurable, with defaults: `serif` → Noto Serif, `sans-serif` → Noto Sans,
`monospace` → Noto Sans Mono, `cursive`/`fantasy` → fallback. The resvg test suite
bundles Noto fonts for this purpose.

**Also**: Tests 007--010 test the font-family fallback list. When the first family isn't
available, the renderer should try subsequent families in order. The current
`FontManager::findFont()` takes a single family string — it needs to accept the full
family list and iterate.

### 2. font-weight: bold rendering -- PARTIAL (7 tests)

Parsing supports `normal` (400), `bold` (700), and numeric 1--1000. `TextShaper` does
weight-matched font selection via `FontManager::findFont(family, weight)`. However:

- **`bolder`/`lighter` keywords not parsed** (same issue as `larger`/`smaller` was for
  font-size — relative keywords need parent context to resolve).
- **Bold font variants not always available**: Weight matching only works against
  registered `@font-face` rules. If no bold variant is registered, the fallback font
  (Public Sans Regular) is used regardless of weight.
- **TextLayout (stb_truetype path) ignores weight entirely** — only TextShaper uses it.

| Test | Pixels | What's tested |
|------|--------|---------------|
| weight-002 | 5,470 | `bold` / `700` |
| weight-003 | 5,470 | `bolder` (relative, unimplemented) |
| weight-004 | 5,470 | `bolder` with clamping at 900 |
| weight-005 | 5,470 | `bolder` without parent |
| weight-006 | 5,470 | `lighter` from parent 800 |
| weight-009 | 2,735 | Numeric `700` |
| weight-012 | 2,719 | Numeric `650` |

**Passing weight tests for reference**: weight-001 (`normal`/`400`), weight-007
(`lighter` clamped at 100), weight-008 (`lighter` without parent → 200), weight-010
(`inherit` from 400), weight-011 (invalid `1500` → fallback). These all resolve to
weight 400 or below, which matches the fallback font.

**What's needed**:
1. Parse `bolder`/`lighter` as relative keywords and resolve during cascade (analogous
   to `larger`/`smaller` for font-size). `bolder` steps up: ≤300→400, ≤500→700,
   >500→900. `lighter` steps down: ≥800→400, ≥600→100, <600→100.
2. Register the test suite's Noto Sans Bold font so weight matching has a bold variant.
3. Optionally: add synthetic bold (wider stroke) as a fallback when no bold variant
   exists.

### 3. font-style -- NOT IMPLEMENTED (3 tests)

`font-style` is listed in `kValidPresentationAttributes` but has no parser in
`kProperties`. Values are stored in `unparsedProperties` and never used. Italic and
oblique rendering is not supported.

| Test | Pixels | What's tested |
|------|--------|---------------|
| style-001 | 2,375 | `italic` |
| style-002 | 2,375 | `oblique` |
| style-003 | 2,375 | `inherit` (italic from parent) |

**What's needed**:
1. Add `font-style` to `kProperties` with parser (values: `normal`, `italic`, `oblique`).
2. Store as an enum in `PropertyRegistry`.
3. Add style parameter to `FontManager::findFont()` for italic variant selection.
4. Optionally: synthetic italic (shear transform) when no italic variant exists.

### 4. font-stretch -- NOT IMPLEMENTED (3 tests)

`font-stretch` is listed in `kValidPresentationAttributes` but has no parser. Not used
in font selection.

| Test | Pixels | What's tested |
|------|--------|---------------|
| stretch-001 | 3,950 | `narrower` |
| stretch-002 | 3,950 | `inherit` (extra-condensed from parent) |
| stretch-003 | 3,950 | `extra-condensed` |

**What's needed**:
1. Add `font-stretch` to `kProperties` with keyword parser (`ultra-condensed` through
   `ultra-expanded`, plus `narrower`/`wider` relative keywords).
2. Store as numeric percentage (50%--200%) in `PropertyRegistry`.
3. Add stretch parameter to `FontManager::findFont()`.
4. Lower priority than font-style since condensed/expanded variants are less common.

### 5. font-variant -- NOT IMPLEMENTED (2 tests)

`font-variant` is listed in `kValidPresentationAttributes` but has no parser. No
small-caps support.

| Test | Pixels | What's tested |
|------|--------|---------------|
| variant-001 | 5,570 | `small-caps` |
| variant-002 | 5,570 | `inherit` (small-caps from parent) |

**What's needed**:
1. Add `font-variant` to `kProperties` with parser (values: `normal`, `small-caps`).
2. For `small-caps`: either use OpenType `smcp` feature (requires HarfBuzz / text-full),
   or synthesize by rendering lowercase chars at ~70% scale with uppercase glyphs.

### 6. font-size: absolute-size keywords -- NOT IMPLEMENTED (3 tests)

`larger`/`smaller` are now parsed, but absolute-size keywords (`xx-small` through
`xx-large`) are not.

| Test | Pixels | What's tested |
|------|--------|---------------|
| size-005 | 4,411 | All named values (xx-small through xx-large) + `larger`/`smaller` + `%` |
| size-008 | 1,010 | `xx-large` without parent |
| size-010 | 4,699 | Parent `font-size="0"`, child absolute `font-size="40"` on tspan |

**What's needed**: Add keyword parsing for `xx-small` (9px), `x-small` (10px), `small`
(13px), `medium` (16px), `large` (18px), `x-large` (24px), `xx-large` (32px). Convert
to `Lengthd(Npx)` at parse time.

Note: size-010 may be a different issue — `font-size="40"` is already absolute, so the
failure may be related to tspan font-size override handling when the parent is zero.

### 7. Residual rendering diffs (3 tests)

These font-size tests pass within the old 17,000px threshold but fail at 100px. The
diffs (4,134--6,265px) are from font rendering engine differences (glyph outlines,
hinting) rather than missing features.

| Test | Pixels | What's tested |
|------|--------|---------------|
| size-007 | 6,265 | `300%` without parent |
| size-016 | 6,138 | `3em` on root `<svg>` |
| size-017 | 4,134 | `5ex` on root `<svg>` |

These use `font-family="Noto Sans"` which is available, and the font-size resolution
is now correct. The remaining diff is inherent to the stb_truetype vs reference renderer
glyph rasterization. These should be given per-test thresholds.

## Writing Mode Gap Analysis {#writing-mode-gaps}

3 failures in the base config. 3 additional tests disabled (016, 017, 019).

### Failures

| Test | Pixels | SVG Pattern | Root Cause |
|------|--------|-------------|------------|
| 010 | 14,562 | `writing-mode="tb"` + `text-anchor="middle"`, font-size 64 | text-anchor centering uses horizontal advances instead of vertical |
| 013 | 3,286 | `writing-mode="tb"` with Japanese + English + Arabic | Script-specific rotation heuristic incorrect for Arabic in vertical mode |
| 020 | 10,599 | `writing-mode="tb"` with `dy="-105"` on second tspan | `dy` applied along Y axis regardless of writing mode; should be inline direction (X in vertical-rl) |

### Root cause analysis

**010 -- text-anchor in vertical mode**: The text-anchor adjustment should operate along
the Y axis (block direction for vertical-rl writing mode), centering the text vertically.
Current implementation computes the centering offset using horizontal advance widths.

**013 -- mixed-script vertical**: In vertical writing mode, CJK characters are rendered
upright while Latin characters are rotated 90 degrees clockwise. The stb_truetype backend
(base config) uses a heuristic based on Unicode code point range (< 0x2E80 = rotate) which
doesn't handle Arabic script correctly. This test passes in text-full config where HarfBuzz
provides proper `vert` GSUB feature access.

**020 -- dy axis in vertical mode**: `dy` in vertical writing mode should shift along the
inline direction (X axis in vertical-rl). Current implementation applies `dy` to Y
regardless of writing mode. The `dy="-105"` should move the second tspan leftward, not
upward.

### Fix plan

**Phase 1**: Fix text-anchor calculation in `TextLayout::layout()` to use vertical advances
when `writing-mode` is vertical. (Fixes 010.)

**Phase 2**: Fix dx/dy axis mapping based on writing mode. (Fixes 020.)

**Phase 3**: Improve mixed-script vertical rotation heuristic for Arabic, or accept as a
base-config limitation resolved by text-full. (Fixes 013 in base config, already passes in
text-full.)
