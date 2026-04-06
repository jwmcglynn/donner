# Design: CSS Fonts Support

**Status:** Partial
**Standard:** [CSS Fonts Level 4](https://www.w3.org/TR/css-fonts-4/)
**Related:** [text/overview.md](text/overview.md)

## Summary

CSS Fonts Level 4 defines font selection, loading, and rendering properties. SVG uses these
as presentation attributes on text elements. This document tracks our support level and
implementation plan for CSS font properties within the donner SVG renderer.

## Current Support

### Implemented Properties

| Property | Status | Notes |
|----------|--------|-------|
| `font-family` | Implemented | Parsed as comma-separated list. Resolved via `FontManager::findFont()` against registered `@font-face` rules. Falls back to embedded Public Sans. |
| `font-size` | Implemented | Supports `px`, `em`, `%`, and other CSS length units. Inherited. Keywords (`small`, `large`, `x-large` etc.) not yet supported. |
| `font-weight` | Implemented | Supports `normal` (400), `bold` (700), and numeric 100-900. Per-span resolution selects weight-matched font from registered `@font-face` rules. Keywords `bolder`/`lighter` not supported. |

### Registered but Not Parsed (stored as unparsed presentation attributes)

| Property | CSS Fonts 4 Section | Priority | Notes |
|----------|---------------------|----------|-------|
| `font-style` | [3.3](https://www.w3.org/TR/css-fonts-4/#font-style-prop) | High | `normal`, `italic`, `oblique [<angle>]`. Needed for italic text rendering. |
| `font-variant` | [3.6](https://www.w3.org/TR/css-fonts-4/#font-variant-prop) | Low | Shorthand for sub-properties (caps, ligatures, etc.). Complex parsing. |
| `font-stretch` | [3.4](https://www.w3.org/TR/css-fonts-4/#font-stretch-prop) | Low | `condensed`, `expanded`, percentage. Rarely used in SVG. |
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

## @font-face Support

### Current State

`@font-face` rules are supported with the following descriptors:

| Descriptor | Status | Notes |
|-----------|--------|-------|
| `font-family` | Implemented | Family name for matching. |
| `src` | Partial | `Data` (inline bytes) supported. `url()` and `local()` not implemented. |
| `font-weight` | Implemented | Stored in `FontFace::fontWeight`. Weight matching in `findFont(family, weight)`. |
| `font-style` | Not implemented | No style matching (italic vs normal). |
| `font-stretch` | Not implemented | No stretch matching. |
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
- Step 2: ❌ Not implemented (always uses first style found)
- Step 3: ✅ Basic closest-weight match via `abs(face.fontWeight - weight)`
- Step 4: ❌ Not implemented

## Implementation Plan

### Phase 1: font-style (High Priority)

Add `font-style` as a CSS property and `@font-face` descriptor.

**PropertyRegistry changes:**
```cpp
Property<FontStyle, PropertyCascade::Inherit> fontStyle{
    "font-style", []() -> std::optional<FontStyle> { return FontStyle::Normal; }};
```

Where `FontStyle` is an enum: `Normal`, `Italic`, `Oblique`.

**FontFace changes:**
```cpp
struct FontFace {
  RcString familyName;
  std::vector<FontFaceSource> sources;
  int fontWeight = 400;
  FontStyle fontStyle = FontStyle::Normal;  // NEW
};
```

**Font matching:**
Update `findFont(family, weight)` → `findFont(family, weight, style)`.

**Test registration:**
Parse style from filename suffix: `NotoSans-Italic.ttf` → `FontStyle::Italic`.

### Phase 2: font shorthand (Medium Priority)

Parse the `font` shorthand property which combines:
`font: [font-style] [font-variant] [font-weight] font-size[/line-height] font-family`

Example: `font: bold 16px "Noto Sans"` → weight=700, size=16px, family="Noto Sans".

### Phase 3: font-feature-settings (Medium Priority)

Pass OpenType feature settings to HarfBuzz via `hb_feature_t`. Parse format:
`font-feature-settings: "liga" 0, "smcp" 1`

**TextShaper changes:**
Store parsed features in `TextParams` and pass to `hb_shape()`:
```cpp
hb_shape(hbFont, chunkBuf, features.data(), features.size());
```

### Phase 4: font-size keywords (Medium Priority)

Support CSS font-size keywords: `xx-small`, `x-small`, `small`, `medium`, `large`,
`x-large`, `xx-large`, `smaller`, `larger`.

Map to pixel values per CSS spec (medium = 16px, each step ~1.2x).

### Phase 5: Variable fonts (Low Priority)

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

Current font-related test results (`--config=text-full`):

| Test Group | Passing | Failing | Notes |
|-----------|---------|---------|-------|
| `a-font-weight-*` | 0 | 6 | All have 18K threshold — our renderer produces different widths |
| `a-font-size-*` | 3 | 8 | Missing keyword support, relative units |
| `e-tspan-006` | ✅ | | Bold tspan now renders with NotoSans-Bold |
| `e-tspan-024` | | 16,722px | Bold + text-anchor interaction |
| `e-tspan-028` | | 9,336px | Mixed font-size in nested tspan |
