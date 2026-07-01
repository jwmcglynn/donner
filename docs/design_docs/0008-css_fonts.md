# Design: CSS Fonts Support

**Status:** Partial — core properties (`font-family`, `font-size`, `font-weight`, `font-style`,
`font-stretch`, `font-variant` small-caps) and matching are implemented; `font` shorthand,
`font-feature-settings`, variable fonts, and `unicode-range` remain unimplemented.
**Standard:** [CSS Fonts Level 4](https://www.w3.org/TR/css-fonts-4/)
**Related:** [text/0052-overview.md](text/0052-overview.md)

## Summary

CSS Fonts Level 4 defines font selection, loading, and rendering properties. SVG uses these
as presentation attributes on text elements. This document tracks our support level and
implementation plan for CSS font properties within the donner SVG renderer.

## Current Support

### Implemented Properties

| Property | Status | Notes |
|----------|--------|-------|
| `font-family` | Implemented | Parsed as comma-separated list. Resolved via `FontManager::findFont()` against registered `@font-face` rules. Falls back to embedded Public Sans. |
| `font-size` | Implemented | Supports `px`, `em`, `%`, and other CSS length units, absolute-size keywords (`xx-small`–`xx-large`), and `larger`/`smaller` relative keywords. Inherited. |
| `font-weight` | Implemented | Supports `normal` (400), `bold` (700), numeric 100-900, and `bolder`/`lighter` relative keywords. Per-span resolution selects weight-matched font from registered `@font-face` rules. |
| `font-style` | Implemented | `normal`, `italic`, `oblique` parsed (`PropertyRegistry`) and used in `FontManager::findFont(family, weight, style, stretch)` matching. |
| `font-stretch` | Implemented | Keyword and percentage values parsed; used in font matching. |
| `font-variant` | Partial | `normal`/`small-caps` parsed; small-caps synthesized in the base tier, native `smcp` OpenType feature used in the full tier. Other CSS Fonts 4 sub-values (`all-small-caps`, `petite-caps`, etc.) not supported. |

### Registered but Not Parsed (stored as unparsed presentation attributes)

| Property | CSS Fonts 4 Section | Priority | Notes |
|----------|---------------------|----------|-------|
| `font-size-adjust` | [3.5](https://www.w3.org/TR/css-fonts-4/#font-size-adjust-prop) | Low | Adjusts x-height across font fallback. Complex metric computation. |

### Not Registered

| Property | CSS Fonts 4 Section | Priority | Notes |
|----------|---------------------|----------|-------|
| `font` | [3.7](https://www.w3.org/TR/css-fonts-4/#font-prop) | Medium | Shorthand (`font: bold 16px/1.2 "Noto Sans"`). Common in CSS stylesheets. |
| `font-synthesis` | [3.8](https://www.w3.org/TR/css-fonts-4/#font-synthesis) | Low | Controls synthetic bold/italic. Not commonly used in SVG. |
| `font-feature-settings` | [6.12](https://www.w3.org/TR/css-fonts-4/#font-feature-settings-prop) | Medium | Direct OpenType feature control (`"liga" 0`, `"smcp" 1`). Passes to HarfBuzz `hb_feature_t`. |
| `font-variation-settings` | [5.2](https://www.w3.org/TR/css-fonts-4/#font-variation-settings-prop) | Low | Variable font axis values. Requires variable font support. |
| `font-optical-sizing` | [5.3](https://www.w3.org/TR/css-fonts-4/#font-optical-sizing) | Low | Automatic optical sizing for variable fonts. |
| `font-palette` | [7.1](https://www.w3.org/TR/css-fonts-4/#font-palette-prop) | Low | Color font palette selection. |
| `line-height` | Related (CSS Inline 3) | Medium | Not in CSS Fonts 4 but essential for multi-line text. |

## `@font-face` Support

### Current State

`@font-face` rules are supported with the following descriptors:

| Descriptor | Status | Notes |
|-----------|--------|-------|
| `font-family` | Implemented | Family name for matching. |
| `src` | Partial | `Data` (inline bytes) supported. `url()` and `local()` not implemented. |
| `font-weight` | Implemented | Stored in `FontFace::fontWeight`. Weight matching in `findFont(family, weight)`. |
| `font-style` | Implemented | Stored in `FontFace::fontStyle`. Style matching in `findFont(family, weight, style, stretch)`. |
| `font-stretch` | Implemented | Stored on `FontFace`. Stretch matching in `findFont(family, weight, style, stretch)`. |
| `font-display` | Not applicable | Only relevant for web loading behavior. |
| `unicode-range` | Not implemented | Would enable per-codepoint font fallback. |

### Font Matching Algorithm

CSS Fonts 4 [Section 4.7](https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm)
defines a multi-step matching algorithm:

1. **Family** — match by `font-family` name
2. **Style** — prefer matching `font-style` (italic, oblique, normal)
3. **Weight** — closest numeric weight match
4. **Stretch** — closest width match

Our current implementation:
- Step 1: ✅ Exact family name match
- Step 2: ✅ Style matching (`findFont(family, weight, style, stretch)`)
- Step 3: ✅ Basic closest-weight match via `abs(face.fontWeight - weight)`
- Step 4: ✅ Stretch matching

## Implementation Plan

Phase 1 (font-style) is complete: `FontStyle` is a `PropertyRegistry` property and a `FontFace`
descriptor, and `FontManager::findFont(family, weight, style, stretch)` matches on it.

### Phase 2: font shorthand (Medium Priority)

Parse the `font` shorthand property which combines:
`font: [font-style] [font-variant] [font-weight] font-size[/line-height] font-family`

Example: `font: bold 16px "Noto Sans"` → weight=700, size=16px, family="Noto Sans".

### Phase 3: font-feature-settings (Medium Priority)

Pass OpenType feature settings to HarfBuzz via `hb_feature_t`. Parse format:
`font-feature-settings: "liga" 0, "smcp" 1`

**`TextBackendFull` changes:**
Store parsed features in `TextLayoutParams` and pass to `hb_shape()`:
```cpp
hb_shape(hbFont, chunkBuf, features.data(), features.size());
```

### Phase 4: Variable fonts (Low Priority)

Support `font-variation-settings` for variable font axes (wght, wdth, ital, etc.).
Requires FreeType's `FT_Set_Var_Design_Coordinates` API.

## Font Fallback

### Current Implementation

Per-script font fallback: when the primary font lacks glyphs for a script (checked via
`FT_Get_Char_Index`), iterates all registered `@font-face` rules to find one with
coverage. This handles cases like "Noto Sans" requested for Arabic text falling back
to Amiri.

### Planned Improvements

1. **`unicode-range` matching** — CSS Fonts 4 `@font-face` `unicode-range` descriptor
   enables per-codepoint font selection without probing each font's cmap.

2. **Per-glyph fallback** — Currently fallback is per-span (entire span uses the
   fallback font). Per-glyph fallback would allow mixed-script text within a single
   span, splitting into font runs.

3. **System font access** — Currently only `@font-face` registered fonts are available.
   System font enumeration would enable `local()` font sources.

## Resvg Test Coverage

Font-related resvg status in the full-text tier (`--config=text-full`). Living
reference: regenerate with
`bazel test //donner/svg/renderer/tests:resvg_test_suite_max` and read
`bazel-testlogs/.../resvg_test_suite_max/test.xml` (enabled = `status="run"`,
disabled = `status="notrun"`). The suite is green, so every enabled test passes.

| Test Group | Enabled passing | Disabled | Notes |
|-----------|-----------------|----------|-------|
| `a-font-weight-*` | 12/12 | 0 | Now passing at default params (the old 18K-threshold override is gone) |
| `a-font-size-*` | 20/20 | 0 | Keyword + relative-unit support landed; `named-value{,-without-a-parent}` pass via blessed goldens (CSS Fonts L4 differs from resvg — see [0009](0009-resvg_test_suite_bugs.md)) |
| `e-tspan-*` | 24/24 | 7 | `e-tspan-006` (bold) and `e-tspan-024` (bold + text-anchor) now pass; `e-tspan-028` (mixed font-size) is among the 7 disabled |
