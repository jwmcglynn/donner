# Text v1 Release

[Back to hub](../0010-text_rendering.md)

**Status:** Shipped

## Overview

Text v1 is Donner's first publishable text release. It covers base-tier `<text>` / `<tspan>`
support, the `text_full` tier (HarfBuzz shaping), core `<textPath>`, and a compliance baseline
against current SVG2 / CSS text-related specs.

### Supported

- `<text>` and `<tspan>` rendering with per-character positioning and rotation
- `text-anchor`, `textLength`, `lengthAdjust`
- `text-decoration` (underline, overline, line-through)
- `dominant-baseline` and `alignment-baseline`
- `@font-face` with TTF, OTF (CFF), WOFF1, WOFF2
- Core `<textPath>` (startOffset, text-anchor, baseline-shift, rotate, transforms, subpaths)
- Base tier (stb_truetype) and full tier (HarfBuzz + FreeType)

### Explicitly Deferred

- Full BiDi text support
- Advanced mixed-script vertical layout
- SVG2 text wrapping (`inline-size`, `shape-inside`)
- SVG2-only textPath features (`side=right`, `path` attribute)
- `font` shorthand, `font-feature-settings`, variable fonts

## Test Coverage

- `e-text-*`: 30/30 passing
- `e-tspan-*`: 24/24 passing
- `a-text-decoration-*`: passing
- `a-lengthAdjust-*`: passing
- `a-dominant-baseline-*`: passing
- `a-textLength-*`: passing
- `a-writing-mode-*`: passing (advanced vertical cases disabled)
- `e-textPath-*`: 32/44 passing (22 custom goldens, 2 thresholds, 8 against resvg reference;
  1 skipped bug, 11 deferred features)

## Normative Spec Baseline

Text v1 uses the latest published W3C text-related specifications as of the release date.
"CSS3" is treated here as the modern family of CSS modules, not a single monolithic spec.

- SVG text elements, DOM APIs, and SVG text attributes:
  SVG 2, 4 October 2018 Candidate Recommendation Snapshot.
  This is the primary SVG text source.
- Font properties and `@font-face`:
  CSS Fonts Module Level 4, 3 March 2026 Working Draft.
  This is the primary source for modern font behavior.
- Text decoration semantics:
  CSS Text Decoration Module Level 3, 5 May 2022 Candidate Recommendation Draft.
  This governs underline/overline/line-through behavior.
- Writing mode and vertical text interaction:
  CSS Writing Modes Level 4, 30 July 2019 Candidate Recommendation Snapshot.
  This governs `writing-mode` and related vertical layout behavior.
- Letter spacing, word spacing, and white-space processing:
  CSS Text Module Level 3, 30 September 2024 Candidate Recommendation Draft.
  This is the primary inline text spacing and whitespace source.
- Forward-looking text processing additions:
  CSS Text Module Level 4, 29 May 2024 Working Draft.
  This is informative for deferred features.
- Inline baseline terminology and alignment model:
  CSS Inline Layout Module Level 3, 18 December 2024 Working Draft.
  This is informative for baseline terminology and alignment references.

## Compliance Matrix

Status legend: **Shipped** = implemented and covered by automated tests, **Partial** = core
behavior works but some sub-features or edge cases are missing, **Deferred** = intentionally
excluded from v1, **OOS** = out of scope (deprecated or not applicable).

#### SVG2 Text (§11) — Primary SVG text source

| Feature | Spec ref | Status | Coverage |
|---------|----------|--------|----------|
| `<text>` element | §11.2 | Shipped | 30/30 `e-text-*` |
| `<tspan>` element | §11.2 | Shipped | 24/24 `e-tspan-*` |
| `<textPath>` element | §11.8 | Shipped | 32/44 `e-textPath-*` (1 bug, 11 deferred) |
| `<tref>` element | — | OOS | Deprecated in SVG2 |
| Per-character positioning (x/y/dx/dy) | §11.4 | Shipped | `e-text-*`, `e-tspan-*` |
| Per-character rotation | §11.4 | Shipped | `e-textPath-029` |
| `text-anchor` | §11.6 | Shipped | `e-text-*`, `e-textPath-019` |
| `textLength` / `lengthAdjust` | §11.5.2 | Shipped | `a-textLength-*`, `a-lengthAdjust-*` |
| `dominant-baseline` | §11.10.2 | Shipped | `a-dominant-baseline-*` |
| `alignment-baseline` | §11.10.2 | Shipped | per-span override |
| `baseline-shift` | §11.10.3 | Shipped | `e-textPath-032` |
| `writing-mode` horizontal | §11.7 | Shipped | `a-writing-mode-*` |
| `writing-mode` vertical | §11.7 | Partial | Basic Latin + CJK; mixed-script gaps |
| `xml:space` | §11.3 | Shipped | `e-text-*` |
| `white-space` CSS | §11.3 | Deferred | SVG2 adds CSS white-space |
| SVG DOM text APIs | §11.5 | Shipped | Unit tests |
| `inline-size` / text wrapping | §11.7.3 | Deferred | SVG2 auto-wrapping |

#### CSS Fonts Level 4

| Feature | Status | Coverage |
|---------|--------|----------|
| `font-family` | Shipped | `a-font-family-*` |
| `font-size` | Shipped | `e-text-*` |
| `font-weight` (100-900, bold/normal) | Shipped | font matching |
| `font-style` (normal, italic, oblique) | Shipped | font matching |
| `font-stretch` | Shipped | font matching |
| `font-variant: small-caps` | Shipped | synthesized (base), native smcp (full) |
| `@font-face` with `src: url()` | Shipped | `a-font-*` |
| TTF, OTF (CFF), WOFF1, WOFF2 | Shipped | font loading tests |
| Generic font families | Shipped | `a-font-family-*` |
| `font` shorthand | Deferred | Individual properties only |
| `font-feature-settings` | Deferred | HarfBuzz supports internally |
| `font-variation-settings` | Deferred | Variable fonts |
| `unicode-range` | Deferred | |

#### CSS Text Decoration Level 3

| Feature | Status | Coverage |
|---------|--------|----------|
| `text-decoration: underline/overline/line-through` | Shipped | `a-text-decoration-*` |
| Multiple decoration values | Shipped | Bitmask parsing |
| Decoration paint from declaring element | Shipped | CSS §3 |
| Decoration stroke | Shipped | |
| `text-decoration-color` | Deferred | Uses fill color |
| `text-decoration-style` | Deferred | Solid only |
| `text-decoration-thickness` | Deferred | Uses font metrics |

#### CSS Writing Modes Level 4

| Feature | Status | Coverage |
|---------|--------|----------|
| `writing-mode: horizontal-tb` | Shipped | Default mode |
| `writing-mode: vertical-rl` | Partial | Basic Latin + CJK |
| Mixed-script vertical text | Deferred | 5 tests skipped |
| `text-orientation` | Deferred | |
| `glyph-orientation-*` | OOS | Deprecated in SVG2 |

#### CSS Text Level 3

| Feature | Status | Coverage |
|---------|--------|----------|
| `letter-spacing` | Shipped | `e-text-*`, `e-textPath-031` |
| `word-spacing` | Shipped | `e-text-*` |
| Whitespace processing (xml:space) | Shipped | `e-text-*` |
| BiDi text | Deferred | 1 test skipped |

## textPath Status

All core textPath features are implemented and tested. 32 of 44 resvg tests pass
(22 with custom goldens for glyph advance drift, 2 with AA thresholds, 8 against
upstream resvg references). 1 test is skipped for a known bug, 11 for deferred features.

### Bugs fixed during v1 stabilization

- **Double text-anchor**: `applyTextAnchor()` was applying a second linear shift to on-path
  glyphs. Fixed by marking path runs with `TextRun.onPath`.
- **Missing kerning on path**: Text-on-path repositioning only used `xAdvance`, losing
  per-glyph kerning. Fixed by preserving `xKern` in `TextGlyph`.
- **Nested textPath leak**: Content inside nested (invalid) `<textPath>` was rendered instead
  of hidden. Fixed in `findApplicableTextPathEntity`.
- **textPath x/y applied**: `<textPath>` x/y/dx/dy/rotate attributes were applied when the
  spec says to ignore them. Fixed by checking `TextPathComponent` in `appendSpan`.
- **dy as perpendicular**: `dy` on tspan within textPath was added to startOffset (along-path)
  instead of perpShift (perpendicular). Fixed.
- **Invalid textPath whitespace**: Hidden textPath content was collapsing surrounding
  inter-element whitespace. Fixed by blocking collapsing across textPath boundaries and
  making hidden spans reset adjacency tracking.
- **Overflow pen position**: When text overflows a path, the pen was set to (0,0) because the
  last glyph was hidden. Fixed by scanning for the last visible glyph.
- **Post-path trailing space**: Trailing whitespace from XML indentation inside `<textPath>`
  was placed on the path. Fixed by removing textPath trailing space during collapsing instead
  of the inter-element space.

### Remaining known bug

| Test | Issue |
|------|-------|
| 012 | Kerning on textPath — mixed textPath/tspan/textPath flat-text continuation |

## Known Gaps

- **e-tspan-030**: Underline color inheritance uses text fill instead of tspan gradient fill.
  Passes with bumped threshold (900).
- **e-textPath-012**: Kerning on textPath with mixed textPath/tspan/textPath continuation.
- **BiDi**: Full bidirectional text support is deferred.
- **Vertical mixed-script**: Advanced mixed-script vertical layout is deferred.
- **SVG2 text wrapping**: `inline-size`, `shape-inside`, `shape-subtract` are deferred.

## Future Work

- [ ] Fix `e-textPath-012` (kerning on textPath).
- [ ] Add deeper dominant-baseline coverage and revisit BASE-table support.
- [ ] Add full BiDi support.
- [ ] Add mixed-script vertical writing-mode parity.
- [ ] Add SVG2 text wrapping.
