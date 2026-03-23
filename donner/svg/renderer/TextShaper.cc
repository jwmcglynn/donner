#include "donner/svg/renderer/TextShaper.h"

#include <ft2build.h>

#include <cmath>
#include <unordered_map>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

#include "donner/base/MathUtils.h"

namespace donner::svg {

namespace {

/// Returns true if the codepoint is non-spacing (does not start a new addressable character).
/// Includes combining marks, zero-width joiners, and variation selectors.
bool isNonSpacing(uint32_t cp) {
  if ((cp >= 0x0300 && cp <= 0x036F) ||  // Combining Diacritical Marks
      (cp >= 0x0483 && cp <= 0x0489) ||  // Cyrillic combining
      (cp >= 0x0591 && cp <= 0x05C7) ||  // Hebrew combining
      (cp >= 0x0610 && cp <= 0x061A) ||  // Arabic combining
      (cp >= 0x064B && cp <= 0x065F) ||  // Arabic combining
      (cp >= 0x0670 && cp == 0x0670) ||  // Arabic superscript alef
      (cp >= 0x06D6 && cp <= 0x06ED) ||  // Arabic combining
      (cp >= 0x0730 && cp <= 0x074A) ||  // Syriac combining
      (cp >= 0x0E31 && cp == 0x0E31) ||  // Thai combining
      (cp >= 0x0E34 && cp <= 0x0E3A) ||  // Thai combining
      (cp >= 0x0EB1 && cp == 0x0EB1) ||  // Lao combining
      (cp >= 0x0EB4 && cp <= 0x0EBC) ||  // Lao combining
      (cp >= 0x1AB0 && cp <= 0x1AFF) ||  // Combining Diacritical Marks Extended
      (cp >= 0x1DC0 && cp <= 0x1DFF) ||  // Combining Diacritical Marks Supplement
      (cp >= 0x20D0 && cp <= 0x20FF) ||  // Combining Diacritical Marks for Symbols
      (cp >= 0xFE20 && cp <= 0xFE2F)) {  // Combining Half Marks
    return true;
  }
  if (cp == 0x200C || cp == 0x200D || cp == 0x034F) {  // ZWNJ, ZWJ, CGJ
    return true;
  }
  if ((cp >= 0xFE00 && cp <= 0xFE0F) ||    // Variation Selectors
      (cp >= 0xE0100 && cp <= 0xE01EF)) {  // Variation Selectors Supplement
    return true;
  }
  return false;
}

/// Decode a single UTF-8 codepoint from a byte offset in a string.
uint32_t decodeCodepointAt(std::string_view str, size_t byteOffset) {
  if (byteOffset >= str.size()) {
    return 0;
  }
  const auto b = static_cast<uint8_t>(str[byteOffset]);
  if (b < 0x80) {
    return b;
  }
  if ((b & 0xE0) == 0xC0 && byteOffset + 1 < str.size()) {
    return (static_cast<uint32_t>(b & 0x1F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F);
  }
  if ((b & 0xF0) == 0xE0 && byteOffset + 2 < str.size()) {
    return (static_cast<uint32_t>(b & 0x0F) << 12) |
           ((static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 2]) & 0x3F);
  }
  if ((b & 0xF8) == 0xF0 && byteOffset + 3 < str.size()) {
    return (static_cast<uint32_t>(b & 0x07) << 18) |
           ((static_cast<uint32_t>(str[byteOffset + 1]) & 0x3F) << 12) |
           ((static_cast<uint32_t>(str[byteOffset + 2]) & 0x3F) << 6) |
           (static_cast<uint32_t>(str[byteOffset + 3]) & 0x3F);
  }
  return 0xFFFD;
}

/// Draw callback context for glyph outline extraction.
struct DrawContext {
  PathSpline* spline = nullptr;
  float scale = 1.0f;
  double curX = 0;
  double curY = 0;
};

// HarfBuzz draw callbacks for glyph outline extraction.
void hbMoveTo(hb_draw_funcs_t* /*dfuncs*/, void* drawData, hb_draw_state_t* /*st*/, float toX,
              float toY, void* /*userData*/) {
  auto* ctx = static_cast<DrawContext*>(drawData);
  const double x = static_cast<double>(toX) * ctx->scale;
  const double y = -static_cast<double>(toY) * ctx->scale;  // Flip Y: font Y-up → SVG Y-down.
  ctx->spline->moveTo(Vector2d(x, y));
  ctx->curX = x;
  ctx->curY = y;
}

void hbLineTo(hb_draw_funcs_t* /*dfuncs*/, void* drawData, hb_draw_state_t* /*st*/, float toX,
              float toY, void* /*userData*/) {
  auto* ctx = static_cast<DrawContext*>(drawData);
  const double x = static_cast<double>(toX) * ctx->scale;
  const double y = -static_cast<double>(toY) * ctx->scale;
  ctx->spline->lineTo(Vector2d(x, y));
  ctx->curX = x;
  ctx->curY = y;
}

void hbQuadTo(hb_draw_funcs_t* /*dfuncs*/, void* drawData, hb_draw_state_t* /*st*/, float ctrlX,
              float ctrlY, float toX, float toY, void* /*userData*/) {
  auto* ctx = static_cast<DrawContext*>(drawData);
  const double cx = static_cast<double>(ctrlX) * ctx->scale;
  const double cy = -static_cast<double>(ctrlY) * ctx->scale;
  const double x = static_cast<double>(toX) * ctx->scale;
  const double y = -static_cast<double>(toY) * ctx->scale;
  // Convert quadratic to cubic: P0 + 2/3*(Pc - P0) and P2 + 2/3*(Pc - P2).
  const double c1x = ctx->curX + (2.0 / 3.0) * (cx - ctx->curX);
  const double c1y = ctx->curY + (2.0 / 3.0) * (cy - ctx->curY);
  const double c2x = x + (2.0 / 3.0) * (cx - x);
  const double c2y = y + (2.0 / 3.0) * (cy - y);
  ctx->spline->curveTo(Vector2d(c1x, c1y), Vector2d(c2x, c2y), Vector2d(x, y));
  ctx->curX = x;
  ctx->curY = y;
}

void hbCubicTo(hb_draw_funcs_t* /*dfuncs*/, void* drawData, hb_draw_state_t* /*st*/, float ctrl1X,
               float ctrl1Y, float ctrl2X, float ctrl2Y, float toX, float toY, void* /*userData*/) {
  auto* ctx = static_cast<DrawContext*>(drawData);
  const double c1x = static_cast<double>(ctrl1X) * ctx->scale;
  const double c1y = -static_cast<double>(ctrl1Y) * ctx->scale;
  const double c2x = static_cast<double>(ctrl2X) * ctx->scale;
  const double c2y = -static_cast<double>(ctrl2Y) * ctx->scale;
  const double x = static_cast<double>(toX) * ctx->scale;
  const double y = -static_cast<double>(toY) * ctx->scale;
  ctx->spline->curveTo(Vector2d(c1x, c1y), Vector2d(c2x, c2y), Vector2d(x, y));
  ctx->curX = x;
  ctx->curY = y;
}

void hbClosePath(hb_draw_funcs_t* /*dfuncs*/, void* drawData, hb_draw_state_t* /*st*/,
                 void* /*userData*/) {
  auto* ctx = static_cast<DrawContext*>(drawData);
  ctx->spline->closePath();
}

/// Thread-local draw funcs singleton (created once, never destroyed — matches HarfBuzz pattern).
hb_draw_funcs_t* getDrawFuncs() {
  static hb_draw_funcs_t* funcs = [] {
    hb_draw_funcs_t* f = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(f, hbMoveTo, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(f, hbLineTo, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(f, hbQuadTo, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(f, hbCubicTo, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(f, hbClosePath, nullptr, nullptr);
    hb_draw_funcs_make_immutable(f);
    return f;
  }();
  return funcs;
}

/// Lazily-initialized FreeType library (process-global, never destroyed).
FT_Library getFtLibrary() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

}  // namespace

/// Internal storage for a HarfBuzz font object backed by FreeType.
/// The hb_font_t is created via hb_ft_font_create_referenced(), which holds a reference to the
/// FT_Face. We also hold our own reference (from FT_New_Memory_Face). Destruction order: destroy
/// hb_font first (decrefs FT_Face from 2→1), then FT_Done_Face (decrefs 1→0, frees).
struct TextShaper::HbFontEntry {
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

TextShaper::TextShaper(FontManager& fontManager) : fontManager_(fontManager) {}

TextShaper::~TextShaper() = default;

hb_font_t* TextShaper::getHbFont(FontHandle handle) {
  if (!handle) {
    return nullptr;
  }

  const auto idx = static_cast<size_t>(handle.index());
  if (idx < hbFonts_.size() && hbFonts_[idx]) {
    return hbFonts_[idx]->font;
  }

  // Create a FreeType face from the raw font data, then wrap it with HarfBuzz.
  // FreeType provides hinted glyph outlines; HarfBuzz provides GSUB/GPOS shaping.
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

  // Set a default size (will be overridden per-layout call).
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

/// Check if a FreeType face has a glyph for the given Unicode codepoint.
bool fontHasCodepoint(FT_Face face, uint32_t codepoint) {
  return FT_Get_Char_Index(face, codepoint) != 0;
}

std::vector<ShapedTextRun> TextShaper::layout(const components::ComputedTextComponent& text,
                                              const TextParams& params) {
  // Resolve font.
  FontHandle font;
  for (const auto& family : params.fontFamilies) {
    font = fontManager_.findFont(family);
    if (font) {
      break;
    }
  }
  if (!font) {
    font = fontManager_.fallbackFont();
  }

  hb_font_t* hbFont = getHbFont(font);
  if (!hbFont) {
    return {};
  }

  // Per-script font fallback: if the primary font is missing glyphs for the text's
  // first non-ASCII codepoint, try all registered fonts to find one with coverage.
  // This handles cases like "Noto Sans" requested for Arabic text — Noto Sans may not
  // include Arabic glyphs, but a registered font like Amiri does.
  if (!text.spans.empty()) {
    const auto& firstSpan = text.spans[0];
    const std::string_view spanText(firstSpan.text.data() + firstSpan.start,
                                    firstSpan.end - firstSpan.start);
    // Find the first non-ASCII codepoint to test coverage.
    uint32_t testCp = 0;
    for (size_t bi = 0; bi < spanText.size();) {
      const uint32_t cp = decodeCodepointAt(spanText, bi);
      if (cp > 0x7F) {
        testCp = cp;
        break;
      }
      const auto byte = static_cast<uint8_t>(spanText[bi]);
      bi += (byte >= 0xF0) ? 4 : (byte >= 0xE0) ? 3 : (byte >= 0xC0) ? 2 : 1;
    }

    if (testCp != 0) {
      FT_Face primaryFace = hb_ft_font_get_face(hbFont);
      if (primaryFace && !fontHasCodepoint(primaryFace, testCp)) {
        // Primary font missing this codepoint. Try all registered fonts.
        for (size_t fi = 0; fi < fontManager_.numFaces(); ++fi) {
          FontHandle candidate = fontManager_.findFont(fontManager_.faceFamilyName(fi));
          if (candidate && candidate != font) {
            hb_font_t* candidateHb = getHbFont(candidate);
            if (candidateHb) {
              FT_Face candidateFace = hb_ft_font_get_face(candidateHb);
              if (candidateFace && fontHasCodepoint(candidateFace, testCp)) {
                font = candidate;
                hbFont = candidateHb;
                break;
              }
            }
          }
        }
      }
    }
  }

  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // Set the FreeType face size for correct metrics at this pixel size.
  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (FT_IS_SCALABLE(ftFace)) {
    FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);
  } else if (ftFace->num_fixed_sizes > 0) {
    FT_Select_Size(ftFace, 0);  // Use the available bitmap strike.
  }
  hb_ft_font_changed(hbFont);

  // With FreeType-backed fonts, HarfBuzz returns positions in 26.6 fixed-point pixels
  // at the face's current size. For scalable fonts that's fontSizePx; for bitmap fonts
  // it's the strike's ppem. Scale accordingly to get document-pixel coordinates.
  double pixelScale = 1.0 / 64.0;
  if (!FT_IS_SCALABLE(ftFace) && ftFace->size->metrics.y_ppem > 0) {
    // Bitmap font: positions are in strike-pixel space. Scale to requested font size.
    pixelScale = static_cast<double>(fontSizePx) /
                 (static_cast<double>(ftFace->size->metrics.y_ppem) * 64.0);
  }

  // Compute dominant-baseline shift.
  double baselineShift = 0.0;
  if (params.dominantBaseline != DominantBaseline::Auto &&
      params.dominantBaseline != DominantBaseline::Alphabetic) {
    hb_font_extents_t extents;
    hb_font_get_h_extents(hbFont, &extents);
    // HarfBuzz: ascender > 0 (above baseline), descender < 0 (below baseline), in font units.
    const double ascent = static_cast<double>(extents.ascender) * pixelScale;
    const double descent = static_cast<double>(extents.descender) * pixelScale;
    switch (params.dominantBaseline) {
      case DominantBaseline::Auto:
      case DominantBaseline::Alphabetic: break;
      case DominantBaseline::Middle:
      case DominantBaseline::Central: baselineShift = (ascent + descent) * 0.5; break;
      case DominantBaseline::Hanging: baselineShift = ascent * 0.8; break;
      case DominantBaseline::Mathematical: baselineShift = ascent * 0.5; break;
      case DominantBaseline::TextTop: baselineShift = ascent; break;
      case DominantBaseline::TextBottom:
      case DominantBaseline::Ideographic: baselineShift = descent; break;
    }
  }

  std::vector<ShapedTextRun> runs;
  bool haveCurrentPosition = false;
  double currentPenX = 0.0;
  double currentPenY = 0.0;
  uint32_t prevSpanLastCodepoint = 0;  // Unicode codepoint, for cross-span GPOS kerning.

  for (const auto& span : text.spans) {
    ShapedTextRun run;

    // Per-span font resolution: if the span's font-weight differs from the default,
    // find a weight-matched font.
    FontHandle spanFont = font;
    if (span.fontWeight != 400) {
      for (const auto& family : params.fontFamilies) {
        FontHandle candidate = fontManager_.findFont(family, span.fontWeight);
        if (candidate) {
          spanFont = candidate;
          break;
        }
      }
    }
    run.font = spanFont;

    // Ensure the HarfBuzz font is created for this span's font.
    hb_font_t* spanHbFont = getHbFont(spanFont);
    if (!spanHbFont) {
      spanHbFont = hbFont;
      run.font = font;
    }
    // Set the FreeType face size for this span's font (may differ from the default font).
    if (spanFont != font && spanHbFont) {
      FT_Face spanFtFace = hb_ft_font_get_face(spanHbFont);
      if (spanFtFace && FT_IS_SCALABLE(spanFtFace)) {
        FT_Set_Char_Size(spanFtFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);
        hb_ft_font_changed(spanHbFont);
      }
    }

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // Resolve per-span baseline-shift using actual font size for em units.
    FontMetrics spanFontMetrics = params.fontMetrics;
    spanFontMetrics.fontSize = fontSizePx;
    const double spanBaselineShiftPx =
        span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);

    // If alignment-baseline is set on this span, it overrides the text-level dominant-baseline.
    double effectiveBaselineShift = baselineShift;
    if (span.alignmentBaseline != DominantBaseline::Auto) {
      effectiveBaselineShift = 0.0;
      hb_font_extents_t extents;
      hb_font_get_h_extents(hbFont, &extents);
      const double ascent = static_cast<double>(extents.ascender) * pixelScale;
      const double descent = static_cast<double>(extents.descender) * pixelScale;
      switch (span.alignmentBaseline) {
        case DominantBaseline::Auto:
        case DominantBaseline::Alphabetic: break;
        case DominantBaseline::Middle:
        case DominantBaseline::Central: effectiveBaselineShift = (ascent + descent) * 0.5; break;
        case DominantBaseline::Hanging: effectiveBaselineShift = ascent * 0.8; break;
        case DominantBaseline::Mathematical: effectiveBaselineShift = ascent * 0.5; break;
        case DominantBaseline::TextTop: effectiveBaselineShift = ascent; break;
        case DominantBaseline::TextBottom:
        case DominantBaseline::Ideographic: effectiveBaselineShift = descent; break;
      }
    }

    double penX = haveCurrentPosition ? currentPenX : 0.0;
    if (span.hasX) {
      penX = span.x.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
    }
    if (span.hasDx) {
      penX += span.dx.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
    }

    const double defaultY = effectiveBaselineShift - spanBaselineShiftPx;
    double penY = haveCurrentPosition ? currentPenY : defaultY;
    if (span.hasY) {
      penY = span.y.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) + defaultY;
    }
    if (span.hasDy) {
      penY += span.dy.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y);
    }

    const double runStartX = penX;
    const double runStartY = penY;

    if (spanText.empty()) {
      currentPenX = penX;
      currentPenY = penY;
      haveCurrentPosition = true;
      runs.push_back(std::move(run));
      continue;
    }

    // Build byte-offset to addressable-character-index map for cluster mapping.
    // The SVG DOM uses UTF-16 indexing for per-character attributes (x, y, dx, dy).
    // Supplementary characters (U+10000+) consume 2 indices (surrogate pair), BMP characters
    // consume 1. Non-spacing characters (combining marks, ZWJ, variation selectors) and
    // characters following a ZWJ don't advance the index.
    //
    // Rotate uses a separate raw-codepoint index where combining marks DO consume values,
    // matching Chrome/resvg behavior.
    std::vector<unsigned int> byteToCharIdx(spanText.size(), 0);
    std::vector<unsigned int> byteToRawCpIdx(spanText.size(), 0);
    {
      unsigned int ci = 0;
      unsigned int rawCi = 0;
      bool firstCodepoint = true;
      bool prevWasZwj = false;
      size_t bi = 0;
      while (bi < spanText.size()) {
        const uint32_t cp = decodeCodepointAt(spanText, bi);
        const bool nonSpacing = isNonSpacing(cp) || prevWasZwj;
        prevWasZwj = (cp == 0x200D);
        if (!nonSpacing && !firstCodepoint) {
          // UTF-16 surrogate pairs consume 2 indices for supplementary characters.
          ci += (cp >= 0x10000) ? 2 : 1;
        }
        if (!firstCodepoint) {
          rawCi += (cp >= 0x10000) ? 2 : 1;
        }
        firstCodepoint = false;

        byteToCharIdx[bi] = ci;
        byteToRawCpIdx[bi] = rawCi;
        const auto byte = static_cast<uint8_t>(spanText[bi]);
        size_t len = 1;
        if (byte >= 0xF0) {
          len = 4;
        } else if (byte >= 0xE0) {
          len = 3;
        } else if (byte >= 0xC0) {
          len = 2;
        }
        for (size_t j = 1; j < len && bi + j < spanText.size(); ++j) {
          byteToCharIdx[bi + j] = ci;
          byteToRawCpIdx[bi + j] = rawCi;
        }
        bi += len;
      }
    }

    const bool vertical = isVertical(params.writingMode);

    const auto& yList = span.yList;
    const auto& xList = span.xList;

    // Per-character coordinates create text chunks. Characters within a chunk
    // connect via cursive joining; characters across chunks don't.
    //
    // Chunk rule: the first explicit per-character y (or x) in visual order defines
    // the initial chunk, absorbing any preceding characters without explicit coords.
    // Each SUBSEQUENT explicit coord starts a new chunk. Characters after the last
    // explicit coord are absorbed into the preceding chunk.
    //
    // Example: yList=[nullopt,150,160,170,nullopt] → chunks {0,1},{2},{3,4}
    //
    // Each chunk is shaped separately via hb_buffer_add_utf8 with item_offset/item_length,
    // which provides the full text as GSUB context but only produces output for the chunk.

    // Build logical byte offset table for visual↔logical mapping.
    std::vector<size_t> logicalByteOffsets;
    {
      size_t bi = 0;
      while (bi < spanText.size()) {
        logicalByteOffsets.push_back(bi);
        const auto byte = static_cast<uint8_t>(spanText[bi]);
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
    }
    const auto cpCount = static_cast<unsigned int>(logicalByteOffsets.size());

    // Detect direction and script from the full text.
    hb_buffer_t* detectBuf = hb_buffer_create();
    hb_buffer_add_utf8(detectBuf, spanText.data(), static_cast<int>(spanText.size()), 0,
                       static_cast<int>(spanText.size()));
    if (vertical) {
      hb_buffer_set_direction(detectBuf, HB_DIRECTION_TTB);
    }
    hb_buffer_guess_segment_properties(detectBuf);
    const hb_direction_t detectedDirection = hb_buffer_get_direction(detectBuf);
    const hb_script_t detectedScript = hb_buffer_get_script(detectBuf);
    const hb_language_t detectedLanguage = hb_buffer_get_language(detectBuf);
    const bool isRTL = detectedDirection == HB_DIRECTION_RTL;
    hb_buffer_destroy(detectBuf);

    // Check if per-character coords exist (will create multiple chunks).
    // When RTL text has per-char coords, Chrome/resvg lay out characters LTR
    // (in DOM order) rather than visual RTL order.
    // Use byteToCharIdx for UTF-16-indexed lookups since xList/yList use UTF-16 indexing.
    bool hasPerCharCoords = false;
    for (unsigned int i = 0; i < cpCount && !hasPerCharCoords; ++i) {
      const unsigned int ci = byteToCharIdx[logicalByteOffsets[i]];
      if ((ci < xList.size() && xList[ci].has_value()) ||
          (ci < yList.size() && yList[ci].has_value())) {
        hasPerCharCoords = true;
      }
    }
    const bool layoutLTR = isRTL && hasPerCharCoords;

    // Build chunk boundaries.
    // When layoutLTR, iterate in logical (DOM) order so chunks process LTR.
    // Otherwise, iterate in visual order for standard RTL shaping.
    // Use byteToCharIdx to map codepoint positions to UTF-16 indices for
    // xList/yList lookup — supplementary characters (emoji) consume 2 UTF-16
    // units per codepoint, so raw codepoint indices don't align with the lists.
    struct ChunkRange {
      unsigned int idxStart = 0;
      unsigned int idxEnd = 0;
    };
    std::vector<ChunkRange> chunks;
    {
      bool seenFirstExplicit = false;
      unsigned int chunkStart = 0;
      for (unsigned int vi = 0; vi < cpCount; ++vi) {
        const unsigned int cpIdx = (isRTL && !layoutLTR) ? (cpCount - 1 - vi) : vi;
        // Map codepoint index to UTF-16 character index for xList/yList.
        const unsigned int ci = byteToCharIdx[logicalByteOffsets[cpIdx]];
        // xList[0]/yList[0] are cleared by TextSystem (scalar span.x/y handles them).
        // The first logical character still has an explicit position if span.hasX/hasY.
        const bool hasExplicit = (ci < xList.size() && xList[ci].has_value()) ||
                                 (ci < yList.size() && yList[ci].has_value()) ||
                                 (cpIdx == 0 && (span.hasX || span.hasY));
        if (hasExplicit) {
          if (!seenFirstExplicit) {
            seenFirstExplicit = true;  // First explicit → defines initial chunk, no split.
          } else {
            chunks.push_back({chunkStart, vi});
            chunkStart = vi;
          }
        }
      }
      chunks.push_back({chunkStart, cpCount});
    }

    // Shape each chunk separately.
    std::vector<hb_glyph_info_t> allInfos;
    std::vector<hb_glyph_position_t> allPositions;
    // For RTL multi-char chunks in layoutLTR mode: override Y for all glyphs
    // in the chunk so they share the chunk's explicit baseline.
    std::unordered_map<unsigned int, double> chunkYOverrides;

    for (const auto& chunk : chunks) {
      // Map chunk range to byte range.
      size_t minByte = spanText.size();
      size_t maxByteEnd = 0;
      for (unsigned int idx = chunk.idxStart; idx < chunk.idxEnd; ++idx) {
        const unsigned int li = (isRTL && !layoutLTR) ? (cpCount - 1 - idx) : idx;
        if (li < cpCount) {
          minByte = std::min(minByte, logicalByteOffsets[li]);
          const size_t nextBo = (li + 1 < cpCount) ? logicalByteOffsets[li + 1] : spanText.size();
          maxByteEnd = std::max(maxByteEnd, nextBo);
        }
      }

      hb_buffer_t* chunkBuf = hb_buffer_create();
      hb_buffer_add_utf8(chunkBuf, spanText.data() + minByte,
                         static_cast<int>(maxByteEnd - minByte), 0,
                         static_cast<int>(maxByteEnd - minByte));
      // When layoutLTR, single-character chunks use LTR for DOM-order output.
      // Multi-character chunks keep the detected direction for correct Arabic
      // joining (e.g., ya-ba connected forms).
      const bool isMultiChar = (chunk.idxEnd - chunk.idxStart) > 1;
      hb_buffer_set_direction(chunkBuf,
                              (layoutLTR && !isMultiChar) ? HB_DIRECTION_LTR : detectedDirection);
      hb_buffer_set_script(chunkBuf, detectedScript);
      hb_buffer_set_language(chunkBuf, detectedLanguage);
      hb_shape(spanHbFont, chunkBuf, nullptr, 0);

      unsigned int chunkGlyphCount = 0;
      const hb_glyph_info_t* chunkInfos = hb_buffer_get_glyph_infos(chunkBuf, &chunkGlyphCount);
      const hb_glyph_position_t* chunkPositions =
          hb_buffer_get_glyph_positions(chunkBuf, &chunkGlyphCount);

      // For RTL multi-char chunks in layoutLTR mode, pre-set penY from the
      // chunk's first logical character (which has the explicit Y). HarfBuzz outputs
      // RTL glyphs in visual order (last-logical first), so the first output glyph
      // would otherwise inherit penY from the previous chunk. By setting penY on the
      // first glyph's cluster target, all glyphs in the chunk share the correct Y.
      // We handle this by overriding penY before the chunk's glyphs are processed:
      // store the chunk's Y in a map from glyph-index to override-Y.
      const unsigned int chunkGlyphStart = static_cast<unsigned int>(allInfos.size());

      for (unsigned int i = 0; i < chunkGlyphCount; ++i) {
        hb_glyph_info_t info = chunkInfos[i];
        // Adjust cluster from chunk-relative to span-relative byte offset.
        info.cluster += static_cast<unsigned int>(minByte);
        allInfos.push_back(info);
        allPositions.push_back(chunkPositions[i]);
      }

      // Record that this multi-char RTL chunk's glyphs should all use the
      // chunk's explicit Y (from its first logical character).
      if (layoutLTR && isMultiChar && chunkGlyphCount > 0) {
        const unsigned int firstLi = chunk.idxStart;
        if (firstLi < yList.size() && yList[firstLi].has_value()) {
          const double chunkY =
              yList[firstLi]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
              baselineShift;
          // Tag all glyphs in this chunk with the override Y by recording the
          // range [chunkGlyphStart, chunkGlyphStart + chunkGlyphCount).
          for (unsigned int gi2 = chunkGlyphStart; gi2 < chunkGlyphStart + chunkGlyphCount; ++gi2) {
            chunkYOverrides[gi2] = chunkY;
          }
        }
      }

      hb_buffer_destroy(chunkBuf);
    }

    // If no per-character coords exist (single chunk = full text), this produces
    // identical results to shaping the full text at once.
    const unsigned int glyphCount = static_cast<unsigned int>(allInfos.size());
    const hb_glyph_info_t* glyphInfos = allInfos.data();
    const hb_glyph_position_t* glyphPositions = allPositions.data();

    for (unsigned int gi = 0; gi < glyphCount; ++gi) {
      const size_t spanByteOffset = glyphInfos[gi].cluster;
      unsigned int charIdx = 0;
      if (spanByteOffset < byteToCharIdx.size()) {
        charIdx = byteToCharIdx[spanByteOffset];
      }

      if (vertical) {
        // Vertical mode: primary advance along Y, cross-axis X.
        // Per-character absolute Y (primary axis).
        if (charIdx < yList.size() && yList[charIdx].has_value()) {
          penY = yList[charIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y);
        }
        if (charIdx < span.dyList.size() && span.dyList[charIdx].has_value()) {
          penY += span.dyList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::Y);
        }
        // Per-character absolute X (cross-axis).
        if (charIdx < xList.size() && xList[charIdx].has_value()) {
          penX = xList[charIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
        }
        if (charIdx < span.dxList.size() && span.dxList[charIdx].has_value()) {
          penX += span.dxList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::X);
        }

        ShapedGlyph glyph;
        glyph.glyphIndex = static_cast<int>(glyphInfos[gi].codepoint);
        // HarfBuzz vertical: y_advance is negative (Y-up convention), negate for SVG Y-down.
        glyph.xPosition = penX + static_cast<double>(glyphPositions[gi].x_offset) * pixelScale;
        glyph.yPosition = penY - static_cast<double>(glyphPositions[gi].y_offset) * pixelScale;
        glyph.yAdvance = std::abs(static_cast<double>(glyphPositions[gi].y_advance) * pixelScale);
        glyph.xAdvance = 0;

        // Per-character rotation using raw codepoint index.
        {
          unsigned int rotIdx = 0;
          if (spanByteOffset < byteToRawCpIdx.size()) {
            rotIdx = byteToRawCpIdx[spanByteOffset];
          }
          if (rotIdx < span.rotateList.size()) {
            glyph.rotateDegrees = span.rotateList[rotIdx];
          } else if (!span.rotateList.empty()) {
            glyph.rotateDegrees = span.rotateList.back();
          } else {
            glyph.rotateDegrees = span.rotateDegrees;
          }
        }

        run.glyphs.push_back(glyph);

        penY += glyph.yAdvance;
        penY += params.letterSpacingPx;
        if (spanByteOffset < spanText.size() && spanText[spanByteOffset] == ' ') {
          penY += params.wordSpacingPx;
        }
      } else {
        // Horizontal mode.
        // For standard RTL (no per-char coords), use visual glyph order (gi).
        // When layoutLTR (per-char coords on RTL text), glyphs are in DOM order
        // so use charIdx.
        const unsigned int posIdx = (isRTL && !layoutLTR) ? gi : charIdx;
        const bool hasAbsoluteX = posIdx < xList.size() && xList[posIdx].has_value();
        const bool hasAbsoluteY = posIdx < yList.size() && yList[posIdx].has_value();

        // Cross-span kerning: HarfBuzz shapes each span separately, so kerning between
        // the last glyph of the previous span and the first of this span is lost. Apply
        // it manually, unless a new text chunk starts (absolute x or y).
        if (gi == 0 && prevSpanLastCodepoint != 0 && !hasAbsoluteX && !hasAbsoluteY) {
          // Cross-span GPOS kerning: shape the pair of Unicode codepoints to get the
          // kerning that HarfBuzz would apply if they were in the same buffer.
          const uint32_t curCodepoint =
              spanByteOffset < spanText.size() ? decodeCodepointAt(spanText, spanByteOffset) : 0;
          if (curCodepoint != 0) {
            hb_buffer_t* pairBuf = hb_buffer_create();
            const uint32_t pair[2] = {prevSpanLastCodepoint, curCodepoint};
            hb_buffer_add_codepoints(pairBuf, pair, 2, 0, 2);
            hb_buffer_set_direction(pairBuf, vertical ? HB_DIRECTION_TTB : HB_DIRECTION_LTR);
            hb_buffer_set_script(pairBuf, HB_SCRIPT_LATIN);
            hb_buffer_guess_segment_properties(pairBuf);
            hb_shape(hbFont, pairBuf, nullptr, 0);
            unsigned int pairCount = 0;
            const hb_glyph_position_t* pairPos = hb_buffer_get_glyph_positions(pairBuf, &pairCount);
            if (pairCount >= 1) {
              // Compare the first glyph's advance in the pair vs standalone.
              const double pairedAdvance = static_cast<double>(pairPos[0].x_advance) * pixelScale;
              // Shape the first character alone to get its standalone advance.
              hb_buffer_t* soloBuf = hb_buffer_create();
              hb_buffer_add_codepoints(soloBuf, &prevSpanLastCodepoint, 1, 0, 1);
              hb_buffer_set_direction(soloBuf, vertical ? HB_DIRECTION_TTB : HB_DIRECTION_LTR);
              hb_buffer_set_script(soloBuf, HB_SCRIPT_LATIN);
              hb_buffer_guess_segment_properties(soloBuf);
              hb_shape(hbFont, soloBuf, nullptr, 0);
              unsigned int soloCount = 0;
              const hb_glyph_position_t* soloPos =
                  hb_buffer_get_glyph_positions(soloBuf, &soloCount);
              if (soloCount >= 1) {
                const double soloAdvance = static_cast<double>(soloPos[0].x_advance) * pixelScale;
                penX += (pairedAdvance - soloAdvance);
              }
              hb_buffer_destroy(soloBuf);
            }
            hb_buffer_destroy(pairBuf);
          }
        }

        // When a new text chunk starts (absolute x or y), suppress cross-chunk kerning.
        // HarfBuzz bakes kerning into x_advance of the previous glyph. If the current
        // glyph starts a new chunk without explicit x, compensate by replacing the
        // previous glyph's shaped advance with its nominal (un-kerned) advance.
        // Skip for RTL: visual-order "previous glyph" is the next logical character,
        // so the kerning between them is correct and shouldn't be suppressed.
        if (!isRTL && !hasAbsoluteX && (hasAbsoluteY) && gi > 0 && !run.glyphs.empty()) {
          // Get the nominal advance (without GPOS kerning) for the previous glyph.
          const unsigned int prevGlyphId = static_cast<unsigned int>(run.glyphs.back().glyphIndex);
          const double nominalAdvance =
              static_cast<double>(hb_font_get_glyph_h_advance(spanHbFont, prevGlyphId)) * pixelScale;
          const double shapedAdvance = run.glyphs.back().xAdvance;
          // The difference is the kerning that shouldn't cross the chunk boundary.
          penX += (nominalAdvance - shapedAdvance);
        }

        if (hasAbsoluteX) {
          penX = xList[posIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
        }

        if (charIdx < span.dxList.size() && span.dxList[charIdx].has_value()) {
          penX += span.dxList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::X);
        }

        // Check for chunk Y override (RTL multi-char chunks in layoutLTR mode).
        auto yOverrideIt = chunkYOverrides.find(gi);
        if (yOverrideIt != chunkYOverrides.end()) {
          penY = yOverrideIt->second;
        } else if (hasAbsoluteY) {
          penY = yList[posIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
                 baselineShift;
        }

        if (charIdx < span.dyList.size() && span.dyList[charIdx].has_value()) {
          penY += span.dyList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::Y);
        }

        ShapedGlyph glyph;
        glyph.glyphIndex = static_cast<int>(glyphInfos[gi].codepoint);
        glyph.xPosition = penX + static_cast<double>(glyphPositions[gi].x_offset) * pixelScale;
        glyph.yPosition = penY - static_cast<double>(glyphPositions[gi].y_offset) * pixelScale;
        glyph.xAdvance = static_cast<double>(glyphPositions[gi].x_advance) * pixelScale;

        // Rotate uses raw codepoint index (combining marks consume values),
        // matching Chrome/resvg behavior.
        {
          unsigned int rotIdx = 0;
          if (spanByteOffset < byteToRawCpIdx.size()) {
            rotIdx = byteToRawCpIdx[spanByteOffset];
          }
          if (rotIdx < span.rotateList.size()) {
            glyph.rotateDegrees = span.rotateList[rotIdx];
          } else if (!span.rotateList.empty()) {
            glyph.rotateDegrees = span.rotateList.back();
          } else {
            glyph.rotateDegrees = span.rotateDegrees;
          }
        }

        // For combining marks (same charIdx as previous glyph, xAdvance == 0),
        // rotate their position offset around the base character's origin so the
        // entire grapheme cluster rotates as a unit per the SVG spec.
        if (glyph.rotateDegrees != 0 && !run.glyphs.empty() && glyph.xAdvance == 0) {
          // Find the base character (last glyph with non-zero advance in this cluster).
          const auto& base = run.glyphs.back();
          if (base.xAdvance != 0) {
            const double angle = glyph.rotateDegrees * MathConstants<double>::kDegToRad;
            const double dx = glyph.xPosition - base.xPosition;
            const double dy = glyph.yPosition - base.yPosition;
            const double cosA = std::cos(angle);
            const double sinA = std::sin(angle);
            glyph.xPosition = base.xPosition + dx * cosA - dy * sinA;
            glyph.yPosition = base.yPosition + dx * sinA + dy * cosA;
          }
        }

        run.glyphs.push_back(glyph);

        // For supplementary characters (UTF-16 surrogate pairs), consume the low
        // surrogate's attribute values. This updates penX/penY for subsequent glyphs
        // without affecting the current glyph's position.
        if (spanByteOffset < spanText.size()) {
          const uint32_t glyphCp = decodeCodepointAt(spanText, spanByteOffset);
          if (glyphCp >= 0x10000) {
            const unsigned int lowIdx = charIdx + 1;
            if (lowIdx < yList.size() && yList[lowIdx].has_value()) {
              penY =
                  yList[lowIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
                  baselineShift;
            }
            if (lowIdx < xList.size() && xList[lowIdx].has_value()) {
              penX =
                  xList[lowIdx]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
            }
          }
        }

        penX += glyph.xAdvance;
        penX += params.letterSpacingPx;
        if (spanByteOffset < spanText.size() && spanText[spanByteOffset] == ' ') {
          penX += params.wordSpacingPx;
        }
      }
    }

    // Save the last Unicode codepoint for cross-span GPOS kerning.
    uint32_t lastCodepoint = 0;
    if (glyphCount > 0) {
      lastCodepoint = decodeCodepointAt(spanText, glyphInfos[glyphCount - 1].cluster);
    }

    // If the span has path data, reposition glyphs along the path.
    if (span.pathSpline && !run.glyphs.empty()) {
      const auto& pathSpline = *span.pathSpline;
      const double startOffset = span.pathStartOffset;

      double advanceAccum = 0.0;
      for (auto& g : run.glyphs) {
        const double glyphMid = startOffset + advanceAccum + g.xAdvance * 0.5;
        const auto sample = pathSpline.pointAtArcLength(glyphMid);

        if (sample.valid) {
          // Shift the glyph origin back along the tangent by half its advance so the
          // glyph's visual center (not its left edge) sits at the path midpoint.
          const double halfAdv = g.xAdvance * 0.5;
          g.xPosition = sample.point.x - halfAdv * std::cos(sample.angle);
          g.yPosition = sample.point.y - halfAdv * std::sin(sample.angle);
          // Combine path tangent angle with per-glyph rotation (already set from per-character
          // rotateList or span.rotateDegrees).
          g.rotateDegrees = sample.angle * MathConstants<double>::kRadToDeg + g.rotateDegrees;
        } else {
          g.glyphIndex = 0;
        }

        advanceAccum += g.xAdvance;
      }

      const auto endSample = pathSpline.pointAtArcLength(startOffset + advanceAccum);
      if (endSample.valid) {
        currentPenX = endSample.point.x;
        currentPenY = endSample.point.y;
        haveCurrentPosition = true;
      } else {
        currentPenX = 0.0;
        currentPenY = 0.0;
        haveCurrentPosition = true;
      }

      runs.push_back(std::move(run));
      continue;
    }

    // Apply textLength adjustment.
    if (params.textLength.has_value() && !run.glyphs.empty()) {
      const double targetLength = params.textLength->toPixels(
          params.viewBox, params.fontMetrics, vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);
      const double naturalLength = vertical ? (penY - runStartY) : (penX - runStartX);

      if (naturalLength > 0.0 && targetLength > 0.0) {
        if (params.lengthAdjust == LengthAdjust::Spacing) {
          const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
          const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
          for (size_t i = 1; i < run.glyphs.size(); ++i) {
            if (vertical) {
              run.glyphs[i].yPosition += extraPerGap * static_cast<double>(i);
            } else {
              run.glyphs[i].xPosition += extraPerGap * static_cast<double>(i);
            }
          }
          if (vertical) {
            penY = runStartY + targetLength;
          } else {
            penX = runStartX + targetLength;
          }
        } else {
          const double scaleFactor = targetLength / naturalLength;
          for (auto& g : run.glyphs) {
            if (vertical) {
              g.yPosition = runStartY + (g.yPosition - runStartY) * scaleFactor;
              g.yAdvance *= scaleFactor;
            } else {
              g.xPosition = runStartX + (g.xPosition - runStartX) * scaleFactor;
              g.xAdvance *= scaleFactor;
            }
          }
          if (vertical) {
            penY = runStartY + targetLength;
          } else {
            penX = runStartX + targetLength;
          }
        }
      }
    }

    // Apply text-anchor adjustment along the inline axis.
    if (params.textAnchor != TextAnchor::Start && !run.glyphs.empty()) {
      const double totalAdvance = vertical ? (penY - runStartY) : (penX - runStartX);
      double shift = 0.0;
      if (params.textAnchor == TextAnchor::Middle) {
        shift = -totalAdvance / 2.0;
      } else if (params.textAnchor == TextAnchor::End) {
        shift = -totalAdvance;
      }
      for (auto& g : run.glyphs) {
        if (vertical) {
          g.yPosition += shift;
        } else {
          g.xPosition += shift;
        }
      }
    }

    currentPenX = penX;
    currentPenY = penY;
    haveCurrentPosition = true;
    prevSpanLastCodepoint = lastCodepoint;
    runs.push_back(std::move(run));
  }

  return runs;
}

PathSpline TextShaper::glyphOutline(FontHandle font, int glyphIndex, float scale) {
  hb_font_t* hbFont = getHbFont(font);
  if (!hbFont) {
    return {};
  }

  // The `scale` parameter is fontSizePx / upem. Convert back to fontSizePx.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  const float fontSizePx = scale * static_cast<float>(upem);

  // Get the FreeType face from HarfBuzz and set it to the correct size.
  FT_Face ftFace = hb_ft_font_get_face(hbFont);
  if (!ftFace) {
    return {};
  }
  FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(fontSizePx * 64.0f), 72, 72);

  // Use NO_HINTING to match the HarfBuzz font configuration (set in getHbFont).
  // Hinted outlines differ from unhinted metrics, causing shape/position mismatches
  // especially for composite glyphs (precomposed accented characters like é).
  if (FT_Load_Glyph(ftFace, static_cast<FT_UInt>(glyphIndex), FT_LOAD_NO_HINTING) != 0) {
    return {};
  }

  if (ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
    return {};
  }

  // Decompose the outline into a PathSpline.
  // FreeType outline coordinates are in 26.6 fixed-point pixels.
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

std::optional<TextShaper::BitmapGlyph> TextShaper::bitmapGlyph(FontHandle font, int glyphIndex,
                                                               float requestedScale) {
  hb_font_t* hbFont = getHbFont(font);
  if (!hbFont) {
    return std::nullopt;
  }

  // Use the entry's FT_Face directly instead of hb_ft_font_get_face, to avoid
  // any state changes HarfBuzz may have applied during shaping.
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

  // The strike ppem determines the native size of the bitmap. If the face has a
  // strike selected, use it; otherwise estimate from bitmap height vs upem.
  // The strike ppem is the em-square size in pixels. The bitmap dimensions may be
  // larger (include padding). Scale the bitmap so the em-square maps to fontSizePx.
  const double strikePpem = static_cast<double>(ftFace->size->metrics.y_ppem);
  const double emScale = strikePpem > 0.0 ? fontSizePx / strikePpem : 1.0;

  // bitmap_top is the distance from baseline to the top of the bitmap, in strike pixels.
  // This matches resvg's formula: translate(x, -height - y) where y = bitmap_top - height.
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

}  // namespace donner::svg
