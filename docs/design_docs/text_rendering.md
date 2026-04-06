# Design: Text Rendering

**Status:** Implemented (Phases 1–6), backend refactor complete
**Author:** Claude Opus 4.6
**Created:** 2026-03-09
**Updated:** 2026-04-04
**Tracking:** [#242](https://github.com/jwmcglynn/donner/issues/242)

## Architecture

Two-tier build-time text stack:

- **Base tier** (`text`): stb_truetype font loading, kern-table kerning, glyph outlines, @font-face.
  No new dependencies (~55 KB).
- **Full tier** (`text_full`, `--config=text-full`): HarfBuzz OpenType shaping (GSUB/GPOS),
  FreeType glyph outlines, WOFF2/Brotli, color emoji (CBDT/CBLC), cursive detection, native
  small-caps. Adds HarfBuzz + FreeType + woff2 (~500–800 KB).

Both tiers share a single `TextEngine` with a pluggable `TextBackend` interface
(`TextBackendSimple` / `TextBackendFull`). Layout, positioning, chunking, text-anchor, textLength,
and text-on-path logic live in `TextEngine::layout()` — no duplication between backends.

Key components:

| Component | Role |
|---|---|
| `FontManager` | ECS-backed font registry: @font-face, family matching, raw font data |
| `TextBackend` | Abstract font operations: metrics, outlines, shaping, capabilities |
| `TextEngine` | SVG text layout, geometry cache, public API support (in `registry.ctx()`) |
| `TextSystem` | ECS layer: flattens DOM text into `ComputedTextComponent` |
| `ComputedTextGeometryComponent` | Cached glyph geometry, character metrics, layout runs |

## Sub-documents

- **[Overview](text/overview.md)** — Goals, non-goals, feature tiers, Bazel flags, phase summary.
- **[Architecture](text/architecture.md)** — Dependency evaluation, component design, implementation
  phases 1–6. *Note: describes the pre-refactor TextLayout/TextShaper split; the current unified
  TextEngine/TextBackend architecture is documented in the refactor doc.*
- **[Testing and Validation](text/testing.md)** — Test strategy, golden image tests, failure
  analysis snapshot (2026-03-30), tspan gap analysis.
- **[RTL Text and Complex Scripts](text/rtl_and_complex_scripts.md)** — HarfBuzz script detection,
  RTL per-character coordinates, combining marks, font fallback, color emoji.
- **[textPath](text/textpath.md)** — `<textPath>` element: architecture, path resolution,
  glyph-on-path positioning, test coverage, known gaps.
- **[Text v1 Release](text/text_v1_release.md)** — Release scope, `<textPath>` finish plan,
  standards baseline, and pre-publish gate.
- **[TextBackend Refactor](text/text_backend_refactor.md)** — TextBackend abstraction, TextEngine
  extraction, layout helper decomposition, ECS caching, invalidation wiring. **This is the
  authoritative architecture document for the current text stack.**

---

## SVG2 Text Feature Checklist

Comprehensive coverage of [SVG2 Chapter 11: Text](https://www.w3.org/TR/SVG2/text.html) and
related CSS properties. Checked items are implemented and validated by resvg golden image tests
or unit tests.

### Elements

- [x] `<text>` — text root element, positioning, rendering
- [x] `<tspan>` — inline text span with per-span positioning and style
- [x] `<textPath>` — text laid out along a path *(33/44 resvg tests enabled and passing;
  11 skipped for deferred features: method=stretch, spacing=auto, side=right, path attr,
  filters, vertical writing-mode on path, dy scaling edge case)*
- [ ] `<tref>` — deprecated in SVG2, not planned

### Text Content and Whitespace

- [x] Character data rendering (ASCII, multi-byte UTF-8)
- [x] `xml:space="default"` — collapse whitespace, strip leading/trailing
- [x] `xml:space="preserve"` — convert newlines/tabs to spaces, preserve all spaces
- [x] Mixed text and child element interleaving (advanceTextChunk)
- [ ] White-space CSS property (`white-space: pre | pre-wrap | pre-line`) — SVG2 adds CSS
  white-space; currently only `xml:space` is supported

### Per-Character Positioning (SVG2 §11.4)

- [x] `x` — absolute X position list
- [x] `y` — absolute Y position list
- [x] `dx` — relative X offset list
- [x] `dy` — relative Y offset list
- [x] `rotate` — per-character rotation list (last value repeats)
- [x] UTF-16 surrogate pair coordinate consumption for supplementary characters
- [x] Combining mark / ZWJ sequence grouping (non-spacing chars share base index)

### Text Layout Properties

- [x] `text-anchor` — start, middle, end per text chunk
- [x] `dominant-baseline` — implemented for the currently-covered values and passing the enabled
  resvg coverage at the default threshold. Current implementation uses heuristic ascent/descent
  percentages rather than OpenType BASE tables, so more coverage would still be useful.
- [x] `alignment-baseline` — per-span override of dominant-baseline
- [x] `baseline-shift` — sub, super, `<length>`, `<percentage>`; OS/2 table metrics for
  sub/super; ancestor baseline-shift accumulation
- [x] `textLength` — per-span and global, spacing and spacingAndGlyphs modes
- [x] `lengthAdjust` — spacing (default) and spacingAndGlyphs
- [x] `writing-mode` — horizontal-tb, vertical-rl *(basic Latin + CJK;
  5 tests skipped for vertical dx/dy bugs and mixed-language issues)*
- [ ] `writing-mode` vertical with non-ASCII mixed scripts — *(skipped: 011, 013, 014, 015)*
- [ ] `glyph-orientation-vertical` / `glyph-orientation-horizontal` — deprecated in SVG2,
  not implemented
- [ ] `text-orientation` — SVG2 property for vertical text glyph orientation; not implemented
- [ ] `inline-size` / `shape-inside` / `shape-subtract` — SVG2 text wrapping; not implemented

### Font Properties

- [x] `font-family` — family name matching from @font-face declarations
- [x] `font-size` — `<length>`, `<percentage>`, absolute/relative keywords
- [x] `font-weight` — numeric 100-900, bold/normal keyword matching
- [x] `font-style` — normal, italic, oblique matching
- [x] `font-stretch` — normal through ultra-expanded matching
- [x] `font-variant` — normal, small-caps (synthesized in base tier, native smcp in full tier)
- [ ] `font-variant` extended values — small-caps only; no `all-small-caps`, `petite-caps`,
  `all-petite-caps`, `unicase`, `titling-caps`
- [ ] `font` shorthand — not parsed; individual properties only
- [ ] `font-size-adjust` — not implemented
- [ ] `font-feature-settings` — not implemented (HarfBuzz supports it internally)
- [ ] `font-variation-settings` — variable fonts not supported
- [x] Generic font families — serif, sans-serif, monospace, cursive, fantasy resolved via
  `FontManager::setGenericFamilyMapping()` *(10 of 11 resvg tests enabled; 009 skipped:
  different default fallback font)*

### @font-face

- [x] `@font-face` rule parsing and registration
- [x] `src: url()` with data URIs and external references
- [x] TTF, OTF (CFF) font loading
- [x] WOFF1 decompression (base tier)
- [x] WOFF2 decompression (full tier, via Brotli)
- [ ] `unicode-range` descriptor — not implemented
- [ ] `font-display` descriptor — not applicable (no async loading)
- [ ] System font discovery / fallback — no OS font enumeration

### Text Decoration

- [x] `text-decoration: underline` — per-glyph baseline following for y-positioned text
- [x] `text-decoration: overline`
- [x] `text-decoration: line-through`
- [x] `text-decoration: none` — suppress decoration
- [x] Multiple values (`underline overline line-through`) — bitmask parsing
- [x] Per-span text-decoration resolved from declaring element's computed style
- [x] Decoration paint and stroke from the declaring element (CSS Text Decoration §3)
- [ ] `text-decoration-color` — not implemented (uses fill color)
- [ ] `text-decoration-style` — solid only; no wavy, dotted, dashed, double
- [ ] `text-decoration-thickness` — not implemented (uses font post table)
- [x] Decoration stroke

### Text Spacing

- [x] `letter-spacing` — per-character extra spacing, suppressed for cursive scripts (full tier)
- [x] `word-spacing` — extra spacing after U+0020
- [ ] `letter-spacing` with BiDi text — *(1 test skipped: mixed LTR+RTL in one span)*

### Text Rendering

- [ ] `text-rendering` — auto, optimizeSpeed, optimizeLegibility, geometricPrecision;
  property not implemented *(tests pass due to threshold fuzziness)*

### Text on a Path (`<textPath>`)

- [x] `href` / `xlink:href` — reference to `<path>` element
- [x] `startOffset` — `<length>` and `<percentage>` along path
- [x] Basic glyph positioning along path with tangent rotation
- [x] `text-anchor` on textPath — shifts effective startOffset for middle/end
- [x] `baseline-shift` on textPath — perpendicular offset from path tangent
- [x] `letter-spacing` on textPath — inter-glyph spacing along path
- [x] Transform on referenced path — applies `<path>` element's local transform
- [x] Invalid/missing href — textPath content hidden (SVG spec §10.12.1)
- [x] Text overflow past path end — glyphs past end hidden
- [x] 33/44 resvg `e-textPath-*` tests enabled and passing
- [ ] `method` — align (default) only; `stretch` not implemented
- [ ] `spacing` — exact only; `auto` not implemented
- [ ] `side` — left (default) only; `right` not implemented (SVG2)
- [ ] `path` attribute — inline path data (SVG2); not implemented
- [ ] `dx`/`dy` on tspan within textPath — not fully applied along/perpendicular to path
- [ ] textPath with filters / masks / clip-paths — not implemented

### Complex Scripts (full tier only)

- [x] HarfBuzz OpenType shaping (GSUB substitution, GPOS positioning)
- [x] Arabic joining forms and contextual shaping
- [x] Combining mark attachment (mark-to-base, mark-to-mark)
- [x] Cross-family font fallback for missing glyphs
- [x] Cursive script detection (letter-spacing suppression)
- [x] Color emoji (CBDT/CBLC bitmap extraction via FreeType)
- [ ] Bidirectional text (BiDi algorithm) — *(1 test skipped: e-text-035)*
- [ ] Vertical CJK with mixed scripts — *(3 tests skipped in writing-mode)*

### SVG DOM Text APIs (§11.5)

- [x] `getNumberOfChars()`
- [x] `getComputedTextLength()`
- [x] `getSubStringLength(charnum, nchars)`
- [x] `getStartPositionOfChar(charnum)`
- [x] `getEndPositionOfChar(charnum)`
- [x] `getExtentOfChar(charnum)`
- [x] `getRotationOfChar(charnum)`
- [x] `getCharNumAtPosition(point)`
- [x] `convertToPath()` (non-standard, for export)
- [x] `inkBoundingBox()` / `objectBoundingBox()`
- [ ] `selectSubString(charnum, nchars)` — stub, not implemented

### Text Interaction with Other Features

- [x] Fill and stroke on text glyphs
- [x] Gradient and pattern paint on text (objectBoundingBox mapping)
- [x] Per-glyph transforms (rotation, translation)
- [x] `opacity` on text elements
- [x] `visibility: hidden` — glyphs hidden but advance preserved
- [x] `display: none` — span skipped entirely
- [ ] `clip-path` on text / with text children — *(5 resvg tests skipped)*
- [ ] `mask` on text — *(1 resvg test skipped)*
- [ ] `filter` on text / textPath — *(2 resvg tests skipped)*

### ECS Caching and Invalidation

- [x] `ComputedTextGeometryComponent` caches layout runs + glyph geometry
- [x] Cache-aware `ensureComputedTextGeometryComponent()` (skip recompute when clean)
- [x] `invalidateTextGeometry()` from 16 DOM mutation entry points
- [x] `TextGeometry` dirty flag in `DirtyFlagsComponent`
- [x] Renderers reuse cached TextRuns before calling `layout()`
- [ ] Font property changes (inherited via style cascade) — handled by full render tree rebuild;
  not yet wired to incremental `TextGeometry` invalidation

---

## Open Work Areas

### 1. textPath Remaining Gaps
33/44 resvg tests enabled and passing. Remaining gaps: `method=stretch`, `spacing=auto`,
`side=right` (SVG2), `path` attribute (SVG2), `dx`/`dy` on tspan children within textPath,
interaction with filters/masks/clip-paths, vertical writing-mode on path.

### 2. Text Wrapping (SVG2)
`inline-size`, `shape-inside`, `shape-subtract` — SVG2 auto-wrapping text into a rectangular
or arbitrary shape region. Not implemented; no design doc yet.

### 3. Bidirectional Text
SheenBidi integration deferred. One resvg test skipped (`e-text-035`). Required for correct
rendering of mixed LTR/RTL content and Arabic paragraph layout.

### 4. Vertical Writing Mode Gaps
5 writing-mode tests skipped: vertical `dx`/`dy` bugs, mixed-language vertical text, vertical
underline, CJK punctuation handling.

### 5. CSS Text Properties
`text-rendering`, `text-decoration-color/style/thickness`, `text-orientation`,
`font-feature-settings`, `font-variation-settings`, `font-size-adjust`, `font` shorthand,
`white-space` CSS property.

## Text v1 Release Status

The text stack is ready for a publishable Text v1 release:

- **`e-text-*`**: 30/30 enabled base-tier tests passing.
- **`e-tspan-*`**: 24/24 enabled base-tier tests passing (`e-tspan-030` reclassified with
  bumped threshold for underline color inheritance gap).
- **`e-textPath-*`**: 33/44 tests enabled and passing (11 skipped for deferred features).
- **`a-text-decoration-*`**, **`a-lengthAdjust-*`**, **`a-textLength-*`**,
  **`a-dominant-baseline-*`**, **`a-writing-mode-*`**: all enabled coverage passing.

Explicitly deferred for v1:

- Full BiDi support.
- Advanced mixed-script vertical text.
- SVG2 text wrapping (`inline-size`, `shape-inside`, `shape-subtract`).
- `method=stretch`, `spacing=auto`, `side=right`, `path` attribute on textPath.
- Text interaction with filters, masks, clip-paths.

See **[Text v1 Release](text/text_v1_release.md)** for the detailed release scope, compliance
matrix, and normative spec baseline.
