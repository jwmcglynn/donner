#include "donner/svg/renderer/TextShaper.h"

#include <hb-ot.h>
#include <hb.h>

#include "donner/base/MathUtils.h"

namespace donner::svg {

namespace {

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
               float ctrl1Y, float ctrl2X, float ctrl2Y, float toX, float toY,
               void* /*userData*/) {
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

}  // namespace

/// Internal storage for a HarfBuzz font object.
struct TextShaper::HbFontEntry {
  hb_blob_t* blob = nullptr;
  hb_face_t* face = nullptr;
  hb_font_t* font = nullptr;

  ~HbFontEntry() {
    if (font) {
      hb_font_destroy(font);
    }
    if (face) {
      hb_face_destroy(face);
    }
    if (blob) {
      hb_blob_destroy(blob);
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

  // Create a new HarfBuzz font from the raw font data.
  const auto data = fontManager_.fontData(handle);
  if (data.empty()) {
    return nullptr;
  }

  auto entry = std::make_unique<HbFontEntry>();
  entry->blob = hb_blob_create(reinterpret_cast<const char*>(data.data()),
                                static_cast<unsigned int>(data.size()), HB_MEMORY_MODE_READONLY,
                                nullptr, nullptr);
  entry->face = hb_face_create(entry->blob, 0);
  entry->font = hb_font_create(entry->face);
  hb_ot_font_set_funcs(entry->font);

  hb_font_t* result = entry->font;

  // Store in cache.
  if (idx >= hbFonts_.size()) {
    hbFonts_.resize(idx + 1);
  }
  hbFonts_[idx] = std::move(entry);

  return result;
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

  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // HarfBuzz works in font units, then we scale to pixels.
  // Get the font's units-per-em to compute the scale factor.
  hb_face_t* face = hb_font_get_face(hbFont);
  const unsigned int upem = hb_face_get_upem(face);
  const double pixelScale = static_cast<double>(fontSizePx) / static_cast<double>(upem);

  // Set the font scale so HarfBuzz positions are in font units at the right scale.
  // HarfBuzz uses 26.6 fixed-point internally, but hb_font_set_scale works in integer font units.
  hb_font_set_scale(hbFont, static_cast<int>(upem), static_cast<int>(upem));

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
      case DominantBaseline::Alphabetic:
        break;
      case DominantBaseline::Middle:
      case DominantBaseline::Central:
        baselineShift = (ascent + descent) * 0.5;
        break;
      case DominantBaseline::Hanging:
        baselineShift = ascent * 0.8;
        break;
      case DominantBaseline::Mathematical:
        baselineShift = ascent * 0.5;
        break;
      case DominantBaseline::TextTop:
        baselineShift = ascent;
        break;
      case DominantBaseline::TextBottom:
      case DominantBaseline::Ideographic:
        baselineShift = descent;
        break;
    }
  }

  std::vector<ShapedTextRun> runs;

  for (const auto& span : text.spans) {
    ShapedTextRun run;
    run.font = font;

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // Resolve span positioning.
    const double baseX =
        span.x.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X) +
        span.dx.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);

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
        case DominantBaseline::Alphabetic:
          break;
        case DominantBaseline::Middle:
        case DominantBaseline::Central:
          effectiveBaselineShift = (ascent + descent) * 0.5;
          break;
        case DominantBaseline::Hanging:
          effectiveBaselineShift = ascent * 0.8;
          break;
        case DominantBaseline::Mathematical:
          effectiveBaselineShift = ascent * 0.5;
          break;
        case DominantBaseline::TextTop:
          effectiveBaselineShift = ascent;
          break;
        case DominantBaseline::TextBottom:
        case DominantBaseline::Ideographic:
          effectiveBaselineShift = descent;
          break;
      }
    }

    const double baseY =
        span.y.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        span.dy.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        effectiveBaselineShift - spanBaselineShiftPx;

    if (spanText.empty()) {
      runs.push_back(std::move(run));
      continue;
    }

    // Build byte-offset to character-index (codepoint index) map for cluster mapping.
    std::vector<unsigned int> byteToCharIdx(spanText.size(), 0);
    {
      unsigned int ci = 0;
      size_t bi = 0;
      while (bi < spanText.size()) {
        byteToCharIdx[bi] = ci;
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
        }
        bi += len;
        ++ci;
      }
    }

    // Create HarfBuzz buffer and shape.
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, spanText.data(), static_cast<int>(spanText.size()), 0,
                       static_cast<int>(spanText.size()));
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_guess_segment_properties(buf);

    hb_shape(hbFont, buf, nullptr, 0);

    // Extract shaped results.
    unsigned int glyphCount = 0;
    const hb_glyph_info_t* glyphInfos = hb_buffer_get_glyph_infos(buf, &glyphCount);
    const hb_glyph_position_t* glyphPositions = hb_buffer_get_glyph_positions(buf, &glyphCount);

    double penX = baseX;
    double penY = baseY;
    for (unsigned int gi = 0; gi < glyphCount; ++gi) {
      // Map glyph to character index via cluster byte offset.
      unsigned int charIdx = 0;
      if (glyphInfos[gi].cluster < byteToCharIdx.size()) {
        charIdx = byteToCharIdx[glyphInfos[gi].cluster];
      }

      // Per-character absolute X positioning overrides the pen.
      const bool hasAbsoluteX =
          charIdx < span.xList.size() && span.xList[charIdx].has_value();
      if (hasAbsoluteX) {
        penX = span.xList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                              Lengthd::Extent::X);
      }

      // Per-character dx.
      if (charIdx < span.dxList.size() && span.dxList[charIdx].has_value()) {
        penX += span.dxList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::X);
      }

      // Per-character absolute Y positioning.
      if (charIdx < span.yList.size() && span.yList[charIdx].has_value()) {
        penY = span.yList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                              Lengthd::Extent::Y) +
               baselineShift;
      }

      // Per-character dy.
      if (charIdx < span.dyList.size() && span.dyList[charIdx].has_value()) {
        penY += span.dyList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::Y);
      }

      ShapedGlyph glyph;
      glyph.glyphIndex = static_cast<int>(glyphInfos[gi].codepoint);
      // HarfBuzz positions are in font units; scale to pixels.
      glyph.xPosition = penX + static_cast<double>(glyphPositions[gi].x_offset) * pixelScale;
      glyph.yPosition = penY - static_cast<double>(glyphPositions[gi].y_offset) * pixelScale;
      glyph.xAdvance = static_cast<double>(glyphPositions[gi].x_advance) * pixelScale;

      // Per-character rotation (last value repeats per SVG spec).
      if (charIdx < span.rotateList.size()) {
        glyph.rotateDegrees = span.rotateList[charIdx];
      } else if (!span.rotateList.empty()) {
        glyph.rotateDegrees = span.rotateList.back();
      } else {
        glyph.rotateDegrees = span.rotateDegrees;
      }

      run.glyphs.push_back(glyph);

      penX += glyph.xAdvance;

      // CSS letter-spacing: extra space after every character.
      penX += params.letterSpacingPx;
      // CSS word-spacing: extra space after U+0020 (space) characters.
      // Use the cluster byte offset to check the original character.
      if (glyphInfos[gi].cluster < spanText.size() && spanText[glyphInfos[gi].cluster] == ' ') {
        penX += params.wordSpacingPx;
      }
    }

    hb_buffer_destroy(buf);

    // If the span has path data, reposition glyphs along the path.
    if (span.pathSpline && !run.glyphs.empty()) {
      const auto& pathSpline = *span.pathSpline;
      const double startOffset = span.pathStartOffset;

      double advanceAccum = 0.0;
      for (auto& g : run.glyphs) {
        const double glyphMid = startOffset + advanceAccum + g.xAdvance * 0.5;
        const auto sample = pathSpline.pointAtArcLength(glyphMid);

        if (sample.valid) {
          g.xPosition = sample.point.x;
          g.yPosition = sample.point.y;
          // Combine path tangent angle with per-glyph rotation (already set from per-character
          // rotateList or span.rotateDegrees).
          g.rotateDegrees = sample.angle * MathConstants<double>::kRadToDeg + g.rotateDegrees;
        } else {
          g.glyphIndex = 0;
        }

        advanceAccum += g.xAdvance;
      }

      runs.push_back(std::move(run));
      continue;
    }

    // Apply textLength adjustment.
    if (params.textLength.has_value() && !run.glyphs.empty()) {
      const double targetLength = params.textLength->toPixels(params.viewBox, params.fontMetrics,
                                                              Lengthd::Extent::X);
      const double naturalLength = penX - baseX;

      if (naturalLength > 0.0 && targetLength > 0.0) {
        if (params.lengthAdjust == LengthAdjust::Spacing) {
          const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
          const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
          for (size_t i = 1; i < run.glyphs.size(); ++i) {
            run.glyphs[i].xPosition += extraPerGap * static_cast<double>(i);
          }
          penX = baseX + targetLength;
        } else {
          const double scaleFactor = targetLength / naturalLength;
          for (auto& g : run.glyphs) {
            g.xPosition = baseX + (g.xPosition - baseX) * scaleFactor;
            g.xAdvance *= scaleFactor;
          }
          penX = baseX + targetLength;
        }
      }
    }

    // Apply text-anchor adjustment.
    if (params.textAnchor != TextAnchor::Start && !run.glyphs.empty()) {
      const double totalAdvance = penX - baseX;
      double shift = 0.0;
      if (params.textAnchor == TextAnchor::Middle) {
        shift = -totalAdvance / 2.0;
      } else if (params.textAnchor == TextAnchor::End) {
        shift = -totalAdvance;
      }
      for (auto& g : run.glyphs) {
        g.xPosition += shift;
      }
    }

    runs.push_back(std::move(run));
  }

  return runs;
}

PathSpline TextShaper::glyphOutline(FontHandle font, int glyphIndex, float scale) {
  hb_font_t* hbFont = getHbFont(font);
  if (!hbFont) {
    return {};
  }

  PathSpline spline;
  DrawContext ctx;
  ctx.spline = &spline;
  ctx.scale = scale;

  hb_font_draw_glyph(hbFont, static_cast<hb_codepoint_t>(glyphIndex), getDrawFuncs(), &ctx);
  return spline;
}

}  // namespace donner::svg
