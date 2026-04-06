# Color Emoji Support

## Overview

Enable rendering of color emoji in SVG `<text>` elements, targeting fonts that use the CBDT/CBLC (Color Bitmap Data Table) format, such as Noto Color Emoji.

## Current State

- **FontManager** uses stb_truetype (`stbtt_InitFont`) to parse all fonts. stb_truetype only supports `glyf`-based (outline) fonts and fails on CBDT-only fonts like NotoColorEmoji.ttf.
- When `stbtt_InitFont` fails, the font is rejected entirely — no `FontHandle` is returned. The font family falls back to Public Sans, and emoji codepoints render as `.notdef` (missing glyph).
- FreeType (available in text-full builds) *can* load CBDT fonts and extract the embedded PNG bitmaps, but it never gets the chance because FontManager rejects the font first.

## Font Format Landscape

Color emoji fonts use one of four table formats:

| Format | Data Type | Used By | FreeType Support |
|--------|-----------|---------|-----------------|
| **CBDT/CBLC** | Embedded PNG bitmaps | Noto Color Emoji, older Android | Yes (`FT_GLYPH_FORMAT_BITMAP`) |
| **COLR/CPAL** v0 | Layered color vector glyphs | Windows emoji, Twemoji | Partial (v0 via FreeType layers API) |
| **COLR** v1 | Gradient/transform-extended vectors | Google Fonts (newer) | Yes (FreeType 2.13+) |
| **SVG** table | Embedded SVG documents per glyph | Firefox emoji, Adobe fonts | No (must parse SVG directly) |

**This design targets CBDT/CBLC only**, which is the format used by the resvg test suite's NotoColorEmoji.ttf. COLR and SVG table support are future work.

## Design

### Scope: text-full builds only

Color emoji requires FreeType for bitmap extraction. Simple text builds (stb_truetype only) cannot support CBDT fonts. The feature is gated behind `DONNER_TEXT_FULL`.

### Architecture Changes

#### 1. FontManager: Accept bitmap-only fonts

When `stbtt_InitFont` fails, check if the font has a valid `head` table (minimal TrueType validity). If so, store the font data and return a valid `FontHandle`, but leave `stbtt_fontinfo` uninitialized.

```cpp
struct LoadedFont {
  // ... existing fields ...
  bool bitmapOnly = false;  // True if stbtt_InitFont failed (CBDT/COLR font).
};
```

- `fontInfo()` returns `nullptr` for bitmap-only fonts (no stb_truetype access).
- `fontData()` still works — returns raw bytes for FreeType/HarfBuzz.
- `scaleForPixelHeight()` reads `head.unitsPerEm` directly for bitmap-only fonts.

#### 2. TextShaper: Bitmap glyph extraction

Add a new method alongside `glyphOutline`:

```cpp
struct BitmapGlyph {
  std::vector<uint8_t> pngData;  // Raw PNG image data.
  int width = 0;
  int height = 0;
  double bearingX = 0;  // Horizontal offset from glyph origin.
  double bearingY = 0;  // Vertical offset from baseline (positive = up).
  double ppem = 0;      // Pixels-per-em the bitmap was designed for.
};

std::optional<BitmapGlyph> TextShaper::bitmapGlyph(FontHandle font, int glyphIndex, float scale);
```

Implementation:
1. Load glyph via `FT_Load_Glyph(face, glyphIndex, FT_LOAD_COLOR)`.
2. Check `face->glyph->format == FT_GLYPH_FORMAT_BITMAP`.
3. Extract bitmap: `face->glyph->bitmap` contains raw pixel data (format `FT_PIXEL_MODE_BGRA` for CBDT).
4. The bitmap's `rows`, `width`, `pitch` give dimensions; `bitmap_left`/`bitmap_top` give bearing.
5. Return the BGRA pixel data (or the raw PNG strike data if available via `FT_Load_Glyph` with `FT_LOAD_NO_BITMAP` flag variations).

Note: FreeType's CBDT implementation decodes the PNG internally and returns BGRA pixels, not raw PNG. The bitmap size depends on the strike selected for the requested ppem.

#### 3. RendererTinySkia: Bitmap glyph rendering

In the glyph rendering loop, after attempting `glyphOutline`:

```cpp
#ifdef DONNER_TEXT_FULL
PathSpline glyphPath = shaper.glyphOutline(run.font, glyph.glyphIndex, scale);
if (glyphPath.empty()) {
  // Try bitmap glyph (color emoji).
  if (auto bitmap = shaper.bitmapGlyph(run.font, glyph.glyphIndex, scale)) {
    // Scale the bitmap from its native ppem to the requested font size.
    // Draw as an RGBA image at the glyph position.
    drawBitmapGlyph(*bitmap, glyph, scale, currentTransform_);
    continue;
  }
}
#endif
```

The bitmap needs to be scaled from its native resolution (typically 128x128 for NotoColorEmoji) to the requested font size. This is a simple image scale + blit operation.

#### 4. Scaling

CBDT fonts contain bitmap strikes at fixed sizes (e.g., 128px for NotoColorEmoji). When rendering at a different size (e.g., 32px), the bitmap must be scaled:

- Scale factor: `requestedSize / strikePpem`
- FreeType selects the best strike automatically via `FT_Select_Size` or the default size selection.
- The renderer scales the bitmap during drawing using the existing image rendering infrastructure (tiny_skia's `drawImageRect`).

### What This Does NOT Support

- **COLR/CPAL** (layered color vectors): Requires iterating color layers via `FT_Get_Color_Glyph_Layer` and rendering each layer with its palette color. Future work.
- **SVG table**: Requires an SVG parser to render per-glyph SVG documents. Complex, future work.
- **Simple text builds**: CBDT requires FreeType. Simple text (stb_truetype only) will continue to fall back to the fallback font for emoji codepoints.
- **Emoji sequences**: ZWJ sequences (e.g., family emoji) require HarfBuzz cluster mapping, which is already handled by the shaping pipeline. The rendering change is per-glyph.

### Test Coverage

Resvg test suite tests that exercise emoji:
- `e-text-027.svg`: Four emoji with Noto Color Emoji (CBDT)
- `e-text-028.svg`: Emoji with Noto Color Emoji, font-size variation
- `e-text-029.svg`: Emoji with Noto Color Emoji, different codepoints

### Migration / Rollout

1. FontManager change is backward-compatible — bitmap-only fonts are only usable in text-full builds.
2. No API changes — emoji rendering is automatic when the font is available.
3. Feature flag: gated behind existing `DONNER_TEXT_FULL` define.

### Estimated Complexity

| Component | Effort | Risk |
|-----------|--------|------|
| FontManager bitmap-only fonts | Small | Low — isolated change |
| TextShaper bitmap extraction | Medium | Medium — FreeType bitmap API |
| RendererTinySkia bitmap drawing | Medium | Low — reuses image drawing |
| Scaling / positioning | Small | Medium — coordinate mapping |
| **Total** | **~Medium** | |
