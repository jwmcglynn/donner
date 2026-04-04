#include "donner/svg/renderer/TextBackendFull.h"

#include <ft2build.h>

#include <algorithm>
#include <cmath>
#include <string>

#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

namespace donner::svg {

namespace {

/// Lazily-initialized FreeType library (process-global, never destroyed).
FT_Library getFtLibrary() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

/// Returns true if the codepoint belongs to a cursive/joining script where letter-spacing
/// should be suppressed (CSS Text §8.1: "cursive scripts ... must not use letter-spacing").
/// This covers Arabic, Syriac, Thaana, N'Ko, Mandaic, and similar connecting scripts.
bool isCursiveScript(uint32_t cp) {
  // Arabic (0600–06FF), Arabic Supplement (0750–077F), Arabic Extended-A (08A0–08FF),
  // Arabic Presentation Forms-A (FB50–FDFF), Arabic Presentation Forms-B (FE70–FEFF).
  if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
      (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
      (cp >= 0xFE70 && cp <= 0xFEFF)) {
    return true;
  }
  // Syriac (0700–074F), Syriac Supplement (0860–086F).
  if ((cp >= 0x0700 && cp <= 0x074F) || (cp >= 0x0860 && cp <= 0x086F)) {
    return true;
  }
  // Thaana (0780–07BF).
  if (cp >= 0x0780 && cp <= 0x07BF) {
    return true;
  }
  // N'Ko (07C0–07FF).
  if (cp >= 0x07C0 && cp <= 0x07FF) {
    return true;
  }
  // Mandaic (0840–085F).
  if (cp >= 0x0840 && cp <= 0x085F) {
    return true;
  }
  return false;
}

/// Small-caps synthesis scale factor.
constexpr float kSmallCapScale = 0.8f;

/// Decode a single UTF-8 codepoint from a byte offset in a string.
uint32_t decodeCodepointAt(std::string_view str, size_t byteOffset) {
  if (byteOffset >= str.size()) {
    return 0;
  }

  const auto byte = static_cast<uint8_t>(str[byteOffset]);
  if (byte < 0x80) {
    return byte;
  }
  if ((byte & 0xE0) == 0xC0 && byteOffset + 1 < str.size()) {
    return (static_cast<uint32_t>(byte & 0x1F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F);
  }
  if ((byte & 0xF0) == 0xE0 && byteOffset + 2 < str.size()) {
    return (static_cast<uint32_t>(byte & 0x0F) << 12) |
           ((static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 2]) & 0x3F);
  }
  if ((byte & 0xF8) == 0xF0 && byteOffset + 3 < str.size()) {
    return (static_cast<uint32_t>(byte & 0x07) << 18) |
           ((static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F) << 12) |
           ((static_cast<uint32_t>(str[byteOffset + 2]) & 0x3F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 3]) & 0x3F);
  }

  return 0xFFFD;
}

}  // namespace

// ---------------------------------------------------------------------------
// HbFontEntry
// ---------------------------------------------------------------------------

/// Internal storage for a HarfBuzz font object backed by FreeType.
/// The hb_font_t is created via hb_ft_font_create_referenced(), which holds a reference to the
/// FT_Face. We also hold our own reference (from FT_New_Memory_Face). Destruction order: destroy
/// hb_font first (decrefs FT_Face from 2->1), then FT_Done_Face (decrefs 1->0, frees).
struct TextBackendFull::HbFontEntry {
  hb_font_t* font = nullptr;
  FT_Face ftFace = nullptr;

  ~HbFontEntry() {
    if (font) {
      hb_font_destroy(font);
    }
    if (ftFace) {
      FT_Done_Face(ftFace);
    }
  }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TextBackendFull::TextBackendFull(FontManager& fontManager) : fontManager_(fontManager) {}

TextBackendFull::~TextBackendFull() = default;

// ---------------------------------------------------------------------------
// Font cache
// ---------------------------------------------------------------------------

hb_font_t* TextBackendFull::getOrCreateHbFont(FontHandle handle) const {
  if (!handle) {
    return nullptr;
  }

  const auto idx = static_cast<size_t>(handle.index());
  if (idx < hbFonts_.size() && hbFonts_[idx]) {
    return hbFonts_[idx]->font;
  }

  // Create a FreeType face from the raw font data, then wrap it with HarfBuzz.
  const auto data = fontManager_.fontData(handle);
  if (data.empty()) {
    return nullptr;
  }

  auto entry = std::make_unique<HbFontEntry>();

  // FT_New_Memory_Face does NOT copy the data — it keeps a pointer. The font data is owned by
  // FontManager (via shared_ptr), so it outlives the FT_Face.
  FT_Error err = FT_New_Memory_Face(getFtLibrary(), data.data(), static_cast<FT_Long>(data.size()),
                                    0, &entry->ftFace);
  if (err != 0) {
    return nullptr;
  }

  // Set a default size (will be overridden per shaping call).
  // For bitmap-only fonts (CBDT), use FT_Select_Size to pick the first strike;
  // FT_Set_Char_Size fails for non-scalable fonts.
  if (entry->ftFace->num_fixed_sizes > 0 && !FT_IS_SCALABLE(entry->ftFace)) {
    FT_Select_Size(entry->ftFace, 0);
  } else {
    FT_Set_Char_Size(entry->ftFace, 0, 16 * 64, 72, 72);
  }

  // Create HarfBuzz font backed by FreeType for GSUB/GPOS shaping.
  entry->font = hb_ft_font_create_referenced(entry->ftFace);

  // Disable hinting for glyph outline extraction so outlines match raw font data
  // (consistent with stb_truetype and resvg's ttf-parser). Hinting changes glyph
  // proportions, causing width/height mismatches against reference renderers.
  hb_ft_font_set_load_flags(entry->font, FT_LOAD_NO_HINTING);

  hb_font_t* result = entry->font;

  // Store in cache.
  if (idx >= hbFonts_.size()) {
    hbFonts_.resize(idx + 1);
  }
  hbFonts_[idx] = std::move(entry);

  return result;
}

// ---------------------------------------------------------------------------
// Font metrics
// ---------------------------------------------------------------------------

FontVMetrics TextBackendFull::fontVMetrics(FontHandle font) const {
  // Read from the raw font data's hhea table to match stb_truetype's behavior.
  // FreeType's FT_Face->ascender may come from OS/2 instead of hhea, causing mismatches.
  const auto data = fontManager_.fontData(font);
  if (data.empty()) {
    return {};
  }

  // Search for the 'hhea' table in the TrueType table directory.
  const auto* d = data.data();
  const int numTables = (d[4] << 8) | d[5];
  for (int i = 0; i < numTables; ++i) {
    const int loc = 12 + 16 * i;
    if (d[loc] == 'h' && d[loc + 1] == 'h' && d[loc + 2] == 'e' && d[loc + 3] == 'a') {
      const int offset = static_cast<int>(
          (static_cast<unsigned>(d[loc + 8]) << 24) | (static_cast<unsigned>(d[loc + 9]) << 16) |
          (static_cast<unsigned>(d[loc + 10]) << 8) | static_cast<unsigned>(d[loc + 11]));
      FontVMetrics metrics;
      metrics.ascent = static_cast<int16_t>((d[offset + 4] << 8) | d[offset + 5]);
      metrics.descent = static_cast<int16_t>((d[offset + 6] << 8) | d[offset + 7]);
      metrics.lineGap = static_cast<int16_t>((d[offset + 8] << 8) | d[offset + 9]);
      return metrics;
    }
  }

  // Fallback to FreeType if hhea not found.
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }
  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return {};
  }
  FontVMetrics metrics;
  metrics.ascent = ftFace->ascender;
  metrics.descent = ftFace->descender;
  metrics.lineGap = ftFace->height - (ftFace->ascender - ftFace->descender);
  return metrics;
}

float TextBackendFull::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  // Match FontManager::scaleForPixelHeight semantics: em-based scaling.
  // pixelHeight / units_per_em maps 1 em to the given pixel height.
  return scaleForEmToPixels(font, pixelHeight);
}

float TextBackendFull::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return 0.0f;
  }

  // Use HarfBuzz's upem (from the head table) instead of FreeType's units_per_EM,
  // because FreeType sets units_per_EM = 0 for non-scalable (bitmap-only) fonts like
  // CBDT color emoji. HarfBuzz reads the head table independently.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  if (upem == 0) {
    return 0.0f;
  }

  return pixelHeight / static_cast<float>(upem);
}

// ---------------------------------------------------------------------------
// Table-derived metrics
// ---------------------------------------------------------------------------

std::optional<UnderlineMetrics> TextBackendFull::underlineMetrics(FontHandle font) const {
  // Read from the raw 'post' table to match TextBackendSimple's behavior exactly.
  // FreeType's FT_Face->underline_position may differ from the raw table values.
  const auto data = fontManager_.fontData(font);
  if (data.empty()) {
    return std::nullopt;
  }

  const auto* d = data.data();
  const int numTables = (d[4] << 8) | d[5];
  for (int i = 0; i < numTables; ++i) {
    const int loc = 12 + 16 * i;
    if (d[loc] == 'p' && d[loc + 1] == 'o' && d[loc + 2] == 's' && d[loc + 3] == 't') {
      const int offset = static_cast<int>(
          (static_cast<unsigned>(d[loc + 8]) << 24) | (static_cast<unsigned>(d[loc + 9]) << 16) |
          (static_cast<unsigned>(d[loc + 10]) << 8) | static_cast<unsigned>(d[loc + 11]));
      UnderlineMetrics metrics;
      metrics.position =
          static_cast<double>(static_cast<int16_t>((d[offset + 8] << 8) | d[offset + 9]));
      metrics.thickness =
          static_cast<double>(static_cast<int16_t>((d[offset + 10] << 8) | d[offset + 11]));
      return metrics;
    }
  }

  return std::nullopt;
}

std::optional<SubSuperMetrics> TextBackendFull::subSuperMetrics(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return std::nullopt;
  }

  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return std::nullopt;
  }

  auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(ftFace, FT_SFNT_OS2));
  if (!os2) {
    return std::nullopt;
  }

  SubSuperMetrics metrics;
  metrics.subscriptYOffset = os2->ySubscriptYOffset;
  metrics.superscriptYOffset = os2->ySuperscriptYOffset;
  return metrics;
}

// ---------------------------------------------------------------------------
// Glyph outline
// ---------------------------------------------------------------------------

PathSpline TextBackendFull::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }

  // The `scale` parameter is fontSizePx / upem (same as stbtt_ScaleForMappingEmToPixels).
  // Convert back to fontSizePx for FreeType.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  const float fontSizePx = scale * static_cast<float>(upem);

  // Get the FreeType face and set it to the correct size.
  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return {};
  }
  FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);

  // Use NO_HINTING to match the HarfBuzz font configuration (set in getOrCreateHbFont).
  // Hinted outlines differ from unhinted metrics, causing shape/position mismatches
  // especially for composite glyphs (precomposed accented characters like e).
  if (FT_Load_Glyph(ftFace, static_cast<FT_UInt>(glyphIndex), FT_LOAD_NO_HINTING) != 0) {
    return {};
  }

  if (ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
    return {};
  }

  // Decompose the outline into a PathSpline.
  // FreeType outline coordinates are in 26.6 fixed-point pixels at the set size.
  // Dividing by 64 gives the pixel coordinates, which match the caller's expected scale
  // because we set FreeType's size to `scale * upem`.
  PathSpline spline;
  const float ftScale = 1.0f / 64.0f;

  FT_Outline_Funcs funcs{};
  struct FtCtx {
    PathSpline* spline;
    float scale;
    double curX = 0.0;
    double curY = 0.0;
    bool contourOpen = false;
  };
  FtCtx ctx{&spline, ftScale};

  funcs.move_to = [](const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    if (c->contourOpen) {
      c->spline->closePath();
    }
    const double x = static_cast<double>(to->x) * c->scale;
    const double y = -static_cast<double>(to->y) * c->scale;
    c->spline->moveTo(Vector2d(x, y));
    c->curX = x;
    c->curY = y;
    c->contourOpen = true;
    return 0;
  };
  funcs.line_to = [](const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double x = static_cast<double>(to->x) * c->scale;
    const double y = -static_cast<double>(to->y) * c->scale;
    c->spline->lineTo(Vector2d(x, y));
    c->curX = x;
    c->curY = y;
    return 0;
  };
  funcs.conic_to = [](const FT_Vector* ctrl, const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double cx = static_cast<double>(ctrl->x) * c->scale;
    const double cy = -static_cast<double>(ctrl->y) * c->scale;
    const double ex = static_cast<double>(to->x) * c->scale;
    const double ey = -static_cast<double>(to->y) * c->scale;
    // Convert quadratic bezier to cubic.
    const double c1x = c->curX + (2.0 / 3.0) * (cx - c->curX);
    const double c1y = c->curY + (2.0 / 3.0) * (cy - c->curY);
    const double c2x = ex + (2.0 / 3.0) * (cx - ex);
    const double c2y = ey + (2.0 / 3.0) * (cy - ey);
    c->spline->curveTo(Vector2d(c1x, c1y), Vector2d(c2x, c2y), Vector2d(ex, ey));
    c->curX = ex;
    c->curY = ey;
    return 0;
  };
  funcs.cubic_to = [](const FT_Vector* ctrl1, const FT_Vector* ctrl2, const FT_Vector* to,
                      void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double ex = static_cast<double>(to->x) * c->scale;
    const double ey = -static_cast<double>(to->y) * c->scale;
    c->spline->curveTo(Vector2d(static_cast<double>(ctrl1->x) * c->scale,
                                -static_cast<double>(ctrl1->y) * c->scale),
                       Vector2d(static_cast<double>(ctrl2->x) * c->scale,
                                -static_cast<double>(ctrl2->y) * c->scale),
                       Vector2d(ex, ey));
    c->curX = ex;
    c->curY = ey;
    return 0;
  };

  FT_Outline_Decompose(&ftFace->glyph->outline, &funcs, &ctx);

  if (ctx.contourOpen) {
    spline.closePath();
  }

  return spline;
}

// ---------------------------------------------------------------------------
// Bitmap-only / bitmap glyph
// ---------------------------------------------------------------------------

bool TextBackendFull::isBitmapOnly(FontHandle font) const {
  // Delegate to FontManager which uses stb_truetype-based detection.
  // stbtt_InitFont fails for CBDT-only fonts (no glyf/CFF data), marking them as bitmap-only.
  // FreeType's FT_IS_SCALABLE would incorrectly report CBDT fonts as scalable since FreeType
  // can handle them, but we need to match the renderer's glyph outline vs bitmap fallback logic.
  return fontManager_.isBitmapOnly(font);
}

std::optional<TextBackend::BitmapGlyph> TextBackendFull::bitmapGlyph(FontHandle font,
                                                                     int glyphIndex,
                                                                     float requestedScale) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return std::nullopt;
  }

  // Use the cached entry's FT_Face directly.
  const auto idx = static_cast<size_t>(font.index());
  if (idx >= hbFonts_.size() || !hbFonts_[idx] || !hbFonts_[idx]->ftFace) {
    return std::nullopt;
  }
  FT_Face ftFace = hbFonts_[idx]->ftFace;

  // For bitmap-only fonts (CBDT), select the best available strike size.
  if (ftFace->num_fixed_sizes > 0) {
    int bestIdx = 0;
    FT_Short bestHeight = 0;
    for (int i = 0; i < ftFace->num_fixed_sizes; ++i) {
      if (ftFace->available_sizes[i].height > bestHeight) {
        bestHeight = ftFace->available_sizes[i].height;
        bestIdx = i;
      }
    }
    FT_Select_Size(ftFace, bestIdx);
  }

  // Request the bitmap with FT_LOAD_COLOR for BGRA bitmaps from CBDT.
  if (FT_Load_Glyph(ftFace, static_cast<FT_UInt>(glyphIndex), FT_LOAD_COLOR) != 0) {
    return std::nullopt;
  }

  if (ftFace->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
    return std::nullopt;
  }

  const FT_Bitmap& bitmap = ftFace->glyph->bitmap;
  if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) {
    return std::nullopt;
  }

  // Convert BGRA to RGBA.
  const int w = static_cast<int>(bitmap.width);
  const int h = static_cast<int>(bitmap.rows);
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);

  for (int row = 0; row < h; ++row) {
    const uint8_t* src = bitmap.buffer + row * bitmap.pitch;
    uint8_t* dst = rgba.data() + row * w * 4;
    for (int col = 0; col < w; ++col) {
      dst[col * 4 + 0] = src[col * 4 + 2];  // R <- B
      dst[col * 4 + 1] = src[col * 4 + 1];  // G <- G
      dst[col * 4 + 2] = src[col * 4 + 0];  // B <- R
      dst[col * 4 + 3] = src[col * 4 + 3];  // A <- A
    }
  }

  // Compute scale: the bitmap was rendered at the strike's ppem, but we want it at
  // the requested font size. The requestedScale is fontSizePx / upem.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  const double fontSizePx = requestedScale * static_cast<double>(upem);

  // The strike ppem determines the native size of the bitmap.
  const double strikePpem = static_cast<double>(ftFace->size->metrics.y_ppem);
  const double emScale = strikePpem > 0.0 ? fontSizePx / strikePpem : 1.0;

  // Pre-scale bearings to document space using fontSize/ppem.
  const double pixelScale = strikePpem > 0.0 ? fontSizePx / strikePpem : 1.0;

  BitmapGlyph result;
  result.rgbaPixels = std::move(rgba);
  result.width = w;
  result.height = h;
  result.bearingX = static_cast<double>(ftFace->glyph->bitmap_left) * pixelScale;
  result.bearingY = static_cast<double>(ftFace->glyph->bitmap_top) * pixelScale;
  result.scale = emScale;
  return result;
}

// ---------------------------------------------------------------------------
// Capability queries
// ---------------------------------------------------------------------------

bool TextBackendFull::isCursive(uint32_t codepoint) const {
  return isCursiveScript(codepoint);
}

bool TextBackendFull::hasSmallCapsFeature(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return false;
  }

  hb_face_t* hbFace = hb_font_get_face(hbFont);
  if (!hbFace) {
    return false;
  }

  unsigned int featureIndex = 0;
  return hb_ot_layout_language_find_feature(hbFace, HB_OT_TAG_GSUB, 0,
                                            HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                            HB_TAG('s', 'm', 'c', 'p'), &featureIndex);
}

// ---------------------------------------------------------------------------
// Shaping
// ---------------------------------------------------------------------------

TextBackend::ShapedRun TextBackendFull::shapeRun(FontHandle font, float fontSizePx,
                                                 std::string_view spanText, size_t byteOffset,
                                                 size_t byteLength, bool isVertical,
                                                 FontVariant fontVariant,
                                                 bool forceLogicalOrder) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }

  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return {};
  }

  // Set the FreeType face size for correct metrics at this pixel size.
  if (FT_IS_SCALABLE(ftFace)) {
    FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);
  } else if (ftFace->num_fixed_sizes > 0) {
    FT_Select_Size(ftFace, 0);
  }
  hb_ft_font_changed(hbFont);

  // With FreeType-backed fonts, HarfBuzz returns positions in 26.6 fixed-point pixels
  // at the face's current size. For scalable fonts that's fontSizePx; for bitmap fonts
  // it's the strike's ppem. Scale accordingly to get document-pixel coordinates.
  double pixelScale = 1.0 / 64.0;
  if (!FT_IS_SCALABLE(ftFace) && ftFace->size->metrics.y_ppem > 0) {
    pixelScale = static_cast<double>(fontSizePx) /
                 (static_cast<double>(ftFace->size->metrics.y_ppem) * 64.0);
  }

  // Detect direction and script from the text chunk.
  const char* chunkData = spanText.data() + byteOffset;
  const int chunkLen = static_cast<int>(byteLength);
  bool useVerticalShaping = false;
  if (isVertical) {
    for (size_t i = 0; i < byteLength;) {
      const uint32_t cp = decodeCodepointAt(std::string_view(chunkData, byteLength), i);
      if (cp >= 0x2E80) {
        useVerticalShaping = true;
        break;
      }

      const auto byte = static_cast<uint8_t>(chunkData[i]);
      if (byte >= 0xF0) {
        i += 4;
      } else if (byte >= 0xE0) {
        i += 3;
      } else if (byte >= 0xC0) {
        i += 2;
      } else {
        i += 1;
      }
    }
  }

  hb_buffer_t* detectBuf = hb_buffer_create();
  hb_buffer_add_utf8(detectBuf, chunkData, chunkLen, 0, chunkLen);
  if (useVerticalShaping) {
    hb_buffer_set_direction(detectBuf, HB_DIRECTION_TTB);
  }
  hb_buffer_guess_segment_properties(detectBuf);
  const hb_direction_t detectedDirection = hb_buffer_get_direction(detectBuf);
  const hb_script_t detectedScript = hb_buffer_get_script(detectBuf);
  const hb_language_t detectedLanguage = hb_buffer_get_language(detectBuf);
  hb_buffer_destroy(detectBuf);

  // Small-caps handling.
  bool useSmcpFeature = false;
  std::string smallCapsText;
  std::vector<bool> isSmallCap;

  if (fontVariant == FontVariant::SmallCaps) {
    // Check for native OpenType small-caps feature.
    hb_face_t* hbFace = hb_font_get_face(hbFont);
    unsigned int featureIndex = 0;
    useSmcpFeature = hbFace && hb_ot_layout_language_find_feature(
                                   hbFace, HB_OT_TAG_GSUB, 0, HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                   HB_TAG('s', 'm', 'c', 'p'), &featureIndex);

    if (!useSmcpFeature) {
      // Synthesize: convert lowercase to uppercase and track which bytes changed.
      const std::string_view chunk(chunkData, byteLength);
      smallCapsText.reserve(byteLength);
      isSmallCap.resize(byteLength, false);
      for (size_t i = 0; i < byteLength; ++i) {
        char ch = chunk[i];
        if (ch >= 'a' && ch <= 'z') {
          smallCapsText.push_back(ch - 'a' + 'A');
          isSmallCap[i] = true;
        } else {
          smallCapsText.push_back(ch);
        }
      }
    }
  }

  // Lambda to shape a byte range and collect results.
  // rangeStart/rangeEnd are byte offsets relative to the start of `text`.
  struct ShapedGlyphInfo {
    hb_glyph_info_t info;
    hb_glyph_position_t pos;
    bool isSynthSmallCap;
  };
  std::vector<ShapedGlyphInfo> allGlyphs;

  auto shapeRange = [&](const char* text, size_t rangeStart, size_t rangeEnd, bool isSmallCapRange,
                        hb_feature_t* features, unsigned int numFeatures) {
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text + rangeStart, static_cast<int>(rangeEnd - rangeStart), 0,
                       static_cast<int>(rangeEnd - rangeStart));

    // For single-codepoint buffers, force LTR direction regardless of detected script.
    // This matches the old TextShaper's layoutLTR behavior: when per-character coordinates
    // split RTL text into individual characters, each is shaped independently in LTR to
    // avoid RTL-specific GPOS adjustments that differ from the connected-text advances.
    const unsigned int bufLen = hb_buffer_get_length(buf);
    hb_buffer_set_direction(
        buf, (!useVerticalShaping && bufLen <= 1) ? HB_DIRECTION_LTR : detectedDirection);
    hb_buffer_set_script(buf, detectedScript);
    hb_buffer_set_language(buf, detectedLanguage);
    hb_shape(hbFont, buf, features, numFeatures);

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);
    for (unsigned int i = 0; i < count; ++i) {
      hb_glyph_info_t info = infos[i];
      info.cluster += static_cast<unsigned int>(rangeStart);
      allGlyphs.push_back({info, positions[i], isSmallCapRange});
    }
    hb_buffer_destroy(buf);
  };

  const char* shapeText = smallCapsText.empty() ? chunkData : smallCapsText.data();

  if (useSmcpFeature) {
    hb_feature_t smcpFeature = {HB_TAG('s', 'm', 'c', 'p'), 1, 0, UINT_MAX};
    shapeRange(chunkData, 0, byteLength, false, &smcpFeature, 1);
  } else if (!isSmallCap.empty()) {
    // Synthesized small-caps: split into sub-runs at small-cap boundaries
    // and shape each at the appropriate font size.
    size_t bi = 0;
    while (bi < byteLength) {
      const bool sc = bi < isSmallCap.size() && isSmallCap[bi];
      const size_t subStart = bi;
      while (bi < byteLength && (bi < isSmallCap.size() && isSmallCap[bi]) == sc) {
        const auto byte = static_cast<uint8_t>(shapeText[bi]);
        if (byte >= 0xF0) {
          bi += 4;
        } else if (byte >= 0xE0) {
          bi += 3;
        } else if (byte >= 0xC0) {
          bi += 2;
        } else {
          bi += 1;
        }
      }

      if (sc) {
        // Shape small-cap sub-run at reduced font size.
        if (FT_IS_SCALABLE(ftFace)) {
          FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * kSmallCapScale * 64.0f),
                           72, 72);
          hb_ft_font_changed(hbFont);
        }

        shapeRange(shapeText, subStart, bi, true, nullptr, 0);

        // Restore full font size.
        if (FT_IS_SCALABLE(ftFace)) {
          FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);
          hb_ft_font_changed(hbFont);
        }
      } else {
        shapeRange(shapeText, subStart, bi, false, nullptr, 0);
      }
    }
  } else {
    shapeRange(chunkData, 0, byteLength, false, nullptr, 0);
  }

  // Convert shaped glyphs to ShapedRun output.
  ShapedRun result;
  result.glyphs.reserve(allGlyphs.size());

  for (const auto& sg : allGlyphs) {
    ShapedGlyph glyph;
    glyph.glyphIndex = static_cast<int>(sg.info.codepoint);
    glyph.xAdvance = static_cast<double>(sg.pos.x_advance) * pixelScale;
    glyph.yAdvance = std::abs(static_cast<double>(sg.pos.y_advance) * pixelScale);
    glyph.xOffset = static_cast<double>(sg.pos.x_offset) * pixelScale;
    glyph.yOffset = -static_cast<double>(sg.pos.y_offset) * pixelScale;
    glyph.xKern = 0;  // Kerning is baked into advances by GPOS.
    glyph.yKern = 0;
    glyph.cluster = sg.info.cluster + static_cast<uint32_t>(byteOffset);
    glyph.fontSizeScale = sg.isSynthSmallCap ? kSmallCapScale : 1.0f;

    if (isVertical && useVerticalShaping) {
      const uint32_t codepoint = decodeCodepointAt(spanText, glyph.cluster);
      const bool sideways = (codepoint > 0 && codepoint < 0x2E80);
      if (sideways) {
        glyph.xAdvance =
            static_cast<double>(hb_font_get_glyph_h_advance(hbFont, sg.info.codepoint)) *
            pixelScale;
        glyph.yAdvance = 0.0;
        glyph.xOffset = 0.0;
        glyph.yOffset = 0.0;
      } else {
        FT_Face verticalFace = hb_ft_font_get_face(hbFont);
        if (verticalFace && verticalFace->units_per_EM > 0) {
          double vertOriginY = glyph.yOffset;
          const double emScale = fontSizePx / static_cast<double>(verticalFace->units_per_EM);
          auto* vhea = static_cast<TT_VertHeader*>(FT_Get_Sfnt_Table(verticalFace, FT_SFNT_VHEA));
          if (vhea && vhea->Ascender > 0) {
            vertOriginY = static_cast<double>(vhea->Ascender) * emScale;
          } else {
            auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(verticalFace, FT_SFNT_OS2));
            if (os2 && os2->sTypoAscender > 0) {
              vertOriginY = static_cast<double>(os2->sTypoAscender) * emScale;
            }
          }
          glyph.yOffset = vertOriginY;
        }
      }
    }

    result.glyphs.push_back(glyph);
  }

  // When forceLogicalOrder is set, sort glyphs by cluster (DOM order).
  // HarfBuzz outputs RTL glyphs in visual order (reversed). Sorting to DOM order
  // allows the engine to process per-character positioning in source text order.
  if (forceLogicalOrder && result.glyphs.size() > 1) {
    std::sort(result.glyphs.begin(), result.glyphs.end(),
              [](const ShapedGlyph& a, const ShapedGlyph& b) { return a.cluster < b.cluster; });
  }

  return result;
}

// ---------------------------------------------------------------------------
// Cross-span kerning
// ---------------------------------------------------------------------------

double TextBackendFull::crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle /*curFont*/,
                                      float /*curSizePx*/, uint32_t prevCodepoint,
                                      uint32_t curCodepoint, bool isVertical) const {
  hb_font_t* hbFont = getOrCreateHbFont(prevFont);
  if (!hbFont) {
    return 0.0;
  }

  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return 0.0;
  }

  // Set the previous span's font size for correct GPOS kerning.
  if (FT_IS_SCALABLE(ftFace)) {
    FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(prevSizePx * 64.0f), 72, 72);
    hb_ft_font_changed(hbFont);
  }

  double pixelScale = 1.0 / 64.0;
  if (!FT_IS_SCALABLE(ftFace) && ftFace->size->metrics.y_ppem > 0) {
    pixelScale = static_cast<double>(prevSizePx) /
                 (static_cast<double>(ftFace->size->metrics.y_ppem) * 64.0);
  }

  // Shape the pair [prev, cur] to get the combined advance.
  hb_buffer_t* pairBuf = hb_buffer_create();
  const uint32_t pair[2] = {prevCodepoint, curCodepoint};
  hb_buffer_add_codepoints(pairBuf, pair, 2, 0, 2);
  hb_buffer_set_direction(pairBuf, isVertical ? HB_DIRECTION_TTB : HB_DIRECTION_LTR);
  hb_buffer_set_script(pairBuf, HB_SCRIPT_LATIN);
  hb_buffer_guess_segment_properties(pairBuf);
  hb_shape(hbFont, pairBuf, nullptr, 0);

  unsigned int pairCount = 0;
  const hb_glyph_position_t* pairPos = hb_buffer_get_glyph_positions(pairBuf, &pairCount);

  double kernDelta = 0.0;
  if (pairCount >= 1) {
    const double pairedAdvance = static_cast<double>(pairPos[0].x_advance) * pixelScale;

    // Shape the previous character alone to get its standalone advance.
    hb_buffer_t* soloBuf = hb_buffer_create();
    hb_buffer_add_codepoints(soloBuf, &prevCodepoint, 1, 0, 1);
    hb_buffer_set_direction(soloBuf, isVertical ? HB_DIRECTION_TTB : HB_DIRECTION_LTR);
    hb_buffer_set_script(soloBuf, HB_SCRIPT_LATIN);
    hb_buffer_guess_segment_properties(soloBuf);
    hb_shape(hbFont, soloBuf, nullptr, 0);

    unsigned int soloCount = 0;
    const hb_glyph_position_t* soloPos = hb_buffer_get_glyph_positions(soloBuf, &soloCount);
    if (soloCount >= 1) {
      const double soloAdvance = static_cast<double>(soloPos[0].x_advance) * pixelScale;
      kernDelta = pairedAdvance - soloAdvance;
    }
    hb_buffer_destroy(soloBuf);
  }
  hb_buffer_destroy(pairBuf);

  return kernDelta;
}

}  // namespace donner::svg
