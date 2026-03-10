#include "donner/svg/renderer/TextLayout.h"

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

namespace donner::svg {

namespace {

/**
 * Decode a single UTF-8 codepoint from a string, advancing the index past the consumed bytes.
 *
 * @param str The UTF-8 string.
 * @param i Current byte index (updated to point past the decoded codepoint).
 * @return The decoded Unicode codepoint, or 0xFFFD on invalid sequences.
 */
uint32_t decodeUtf8(const std::string_view str, size_t& i) {
  const auto byte = static_cast<uint8_t>(str[i]);

  if (byte < 0x80) {
    i += 1;
    return byte;
  }
  if ((byte & 0xE0) == 0xC0) {
    if (i + 1 >= str.size()) {
      i = str.size();
      return 0xFFFD;
    }
    const uint32_t cp = (static_cast<uint32_t>(byte & 0x1F) << 6) |
                        (static_cast<uint32_t>(str[i + 1]) & 0x3F);
    i += 2;
    return cp;
  }
  if ((byte & 0xF0) == 0xE0) {
    if (i + 2 >= str.size()) {
      i = str.size();
      return 0xFFFD;
    }
    const uint32_t cp = (static_cast<uint32_t>(byte & 0x0F) << 12) |
                        ((static_cast<uint32_t>(str[i + 1]) & 0x3F) << 6) |
                        (static_cast<uint32_t>(str[i + 2]) & 0x3F);
    i += 3;
    return cp;
  }
  if ((byte & 0xF8) == 0xF0) {
    if (i + 3 >= str.size()) {
      i = str.size();
      return 0xFFFD;
    }
    const uint32_t cp = (static_cast<uint32_t>(byte & 0x07) << 18) |
                        ((static_cast<uint32_t>(str[i + 1]) & 0x3F) << 12) |
                        ((static_cast<uint32_t>(str[i + 2]) & 0x3F) << 6) |
                        (static_cast<uint32_t>(str[i + 3]) & 0x3F);
    i += 4;
    return cp;
  }

  // Invalid leading byte.
  i += 1;
  return 0xFFFD;
}

}  // namespace

TextLayout::TextLayout(FontManager& fontManager) : fontManager_(fontManager) {}

std::vector<LayoutTextRun> TextLayout::layout(const components::ComputedTextComponent& text,
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

  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return {};
  }

  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
  const float scale = fontManager_.scaleForPixelHeight(font, fontSizePx);

  // Compute dominant-baseline shift using font vertical metrics.
  // The shift moves the alphabetic baseline so that the dominant baseline sits at y.
  double baselineShift = 0.0;
  if (params.dominantBaseline != DominantBaseline::Auto &&
      params.dominantBaseline != DominantBaseline::Alphabetic) {
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
    // ascent > 0 (above baseline), descent < 0 (below baseline), in font units.
    switch (params.dominantBaseline) {
      case DominantBaseline::Auto:
      case DominantBaseline::Alphabetic:
        break;
      case DominantBaseline::Middle:
      case DominantBaseline::Central:
        // Center of the em box: (ascent + descent) / 2 above the alphabetic baseline.
        baselineShift = static_cast<double>(ascent + descent) * 0.5 * scale;
        break;
      case DominantBaseline::Hanging:
        // Hanging baseline: approximately 80% of ascent above alphabetic.
        baselineShift = static_cast<double>(ascent) * 0.8 * scale;
        break;
      case DominantBaseline::Mathematical:
        // Mathematical baseline: approximately 50% of ascent above alphabetic.
        baselineShift = static_cast<double>(ascent) * 0.5 * scale;
        break;
      case DominantBaseline::TextTop:
        // Top of em box: ascent above alphabetic.
        baselineShift = static_cast<double>(ascent) * scale;
        break;
      case DominantBaseline::TextBottom:
      case DominantBaseline::Ideographic:
        // Bottom of em box / ideographic baseline: descent below alphabetic.
        baselineShift = static_cast<double>(descent) * scale;
        break;
    }
  }

  std::vector<LayoutTextRun> runs;

  for (const auto& span : text.spans) {
    LayoutTextRun run;
    run.font = font;

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // Resolve span positioning.
    const double baseX =
        span.x.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X) +
        span.dx.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
    const double baseY =
        span.y.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        span.dy.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
        baselineShift;

    double penX = baseX;

    // Decode each codepoint, look up glyph, accumulate advance + kerning.
    size_t byteIdx = 0;
    int prevGlyph = 0;
    while (byteIdx < spanText.size()) {
      const uint32_t codepoint = decodeUtf8(spanText, byteIdx);
      const int glyphIndex = stbtt_FindGlyphIndex(info, static_cast<int>(codepoint));

      // Kern adjustment with previous glyph.
      if (prevGlyph != 0 && glyphIndex != 0) {
        const int kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, glyphIndex);
        penX += static_cast<double>(kern) * scale;
      }

      // Advance width.
      int advanceWidth = 0;
      int leftSideBearing = 0;
      stbtt_GetGlyphHMetrics(info, glyphIndex, &advanceWidth, &leftSideBearing);

      LayoutGlyph glyph;
      glyph.glyphIndex = glyphIndex;
      glyph.xPosition = penX;
      glyph.yPosition = baseY;
      glyph.xAdvance = static_cast<double>(advanceWidth) * scale;
      glyph.rotateDegrees = span.rotateDegrees;
      run.glyphs.push_back(glyph);

      penX += glyph.xAdvance;
      prevGlyph = glyphIndex;
    }

    // Apply textLength adjustment: stretch or compress glyph positions to fit the target length.
    if (params.textLength.has_value() && !run.glyphs.empty()) {
      const double targetLength = params.textLength->toPixels(params.viewBox, params.fontMetrics,
                                                              Lengthd::Extent::X);
      const double naturalLength = penX - baseX;

      if (naturalLength > 0.0 && targetLength > 0.0) {
        if (params.lengthAdjust == LengthAdjust::Spacing) {
          // Distribute extra space evenly between glyphs.
          const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
          const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
          for (size_t gi = 1; gi < run.glyphs.size(); ++gi) {
            run.glyphs[gi].xPosition += extraPerGap * static_cast<double>(gi);
          }
          // Update penX to reflect the new end position.
          penX = baseX + targetLength;
        } else {
          // SpacingAndGlyphs: scale all positions and advances uniformly.
          const double scaleFactor = targetLength / naturalLength;
          for (auto& g : run.glyphs) {
            g.xPosition = baseX + (g.xPosition - baseX) * scaleFactor;
            g.xAdvance *= scaleFactor;
          }
          penX = baseX + targetLength;
        }
      }
    }

    // Apply text-anchor adjustment: shift all glyph positions based on total advance.
    if (params.textAnchor != TextAnchor::Start && !run.glyphs.empty()) {
      // Total advance is the distance from the first glyph to the end of the last glyph.
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

PathSpline glyphToPathSpline(const stbtt_fontinfo* info, int glyphIndex, float scale) {
  stbtt_vertex* vertices = nullptr;
  const int numVertices = stbtt_GetGlyphShape(info, glyphIndex, &vertices);

  PathSpline spline;
  if (numVertices <= 0 || vertices == nullptr) {
    return spline;
  }

  // Track the current point for quad→cubic conversion.
  double curX = 0;
  double curY = 0;

  for (int i = 0; i < numVertices; ++i) {
    const double x = static_cast<double>(vertices[i].x) * scale;
    // stb_truetype Y is up, SVG Y is down — flip.
    const double y = -static_cast<double>(vertices[i].y) * scale;

    switch (vertices[i].type) {
      case STBTT_vmove:
        spline.moveTo(Vector2d(x, y));
        curX = x;
        curY = y;
        break;

      case STBTT_vline:
        spline.lineTo(Vector2d(x, y));
        curX = x;
        curY = y;
        break;

      case STBTT_vcurve: {
        // Quadratic bezier → convert to cubic.
        // Quad control point.
        const double cx = static_cast<double>(vertices[i].cx) * scale;
        const double cy = -static_cast<double>(vertices[i].cy) * scale;
        // Cubic control points: P0 + 2/3*(Pc - P0) and P2 + 2/3*(Pc - P2).
        const double c1x = curX + (2.0 / 3.0) * (cx - curX);
        const double c1y = curY + (2.0 / 3.0) * (cy - curY);
        const double c2x = x + (2.0 / 3.0) * (cx - x);
        const double c2y = y + (2.0 / 3.0) * (cy - y);
        spline.curveTo(Vector2d(c1x, c1y), Vector2d(c2x, c2y), Vector2d(x, y));
        curX = x;
        curY = y;
        break;
      }

      case STBTT_vcubic: {
        const double cx = static_cast<double>(vertices[i].cx) * scale;
        const double cy = -static_cast<double>(vertices[i].cy) * scale;
        const double cx1 = static_cast<double>(vertices[i].cx1) * scale;
        const double cy1 = -static_cast<double>(vertices[i].cy1) * scale;
        spline.curveTo(Vector2d(cx, cy), Vector2d(cx1, cy1), Vector2d(x, y));
        curX = x;
        curY = y;
        break;
      }

      default:
        break;
    }
  }

  stbtt_FreeShape(info, vertices);
  return spline;
}

}  // namespace donner::svg
