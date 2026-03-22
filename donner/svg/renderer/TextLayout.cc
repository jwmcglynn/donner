#include "donner/svg/renderer/TextLayout.h"

#include <cmath>

#include "donner/base/MathUtils.h"

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

/**
 * Returns true if the codepoint is a Unicode combining mark (General Category M).
 * Combining marks should not consume per-character attribute slots (x, y, rotate, etc.)
 * since they form grapheme clusters with their preceding base character.
 */
/**
 * Returns true if the codepoint is a non-spacing character that does not start a new
 * addressable character (grapheme cluster) for SVG per-character attributes.
 * Includes combining marks, zero-width joiners, and variation selectors.
 */
bool isNonSpacing(uint32_t cp) {
  // Combining marks (General Category M).
  if ((cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
      (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic combining
      (cp >= 0x0591 && cp <= 0x05C7) ||   // Hebrew combining
      (cp >= 0x0610 && cp <= 0x061A) ||   // Arabic combining
      (cp >= 0x064B && cp <= 0x065F) ||   // Arabic combining
      (cp >= 0x0670 && cp == 0x0670) ||   // Arabic superscript alef
      (cp >= 0x06D6 && cp <= 0x06ED) ||   // Arabic combining
      (cp >= 0x0730 && cp <= 0x074A) ||   // Syriac combining
      (cp >= 0x0E31 && cp == 0x0E31) ||   // Thai combining
      (cp >= 0x0E34 && cp <= 0x0E3A) ||   // Thai combining
      (cp >= 0x0EB1 && cp == 0x0EB1) ||   // Lao combining
      (cp >= 0x0EB4 && cp <= 0x0EBC) ||   // Lao combining
      (cp >= 0x1AB0 && cp <= 0x1AFF) ||   // Combining Diacritical Marks Extended
      (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Combining Diacritical Marks Supplement
      (cp >= 0x20D0 && cp <= 0x20FF) ||   // Combining Diacritical Marks for Symbols
      (cp >= 0xFE20 && cp <= 0xFE2F)) {   // Combining Half Marks
    return true;
  }

  // Zero-width joiners and format characters used in emoji/ligature sequences.
  if (cp == 0x200C ||                      // Zero Width Non-Joiner
      cp == 0x200D ||                      // Zero Width Joiner (emoji ZWJ sequences)
      cp == 0x034F) {                      // Combining Grapheme Joiner
    return true;
  }

  // Variation selectors (emoji style selectors, ideographic variation sequences).
  if ((cp >= 0xFE00 && cp <= 0xFE0F) ||   // Variation Selectors (VS1-VS16)
      (cp >= 0xE0100 && cp <= 0xE01EF)) {  // Variation Selectors Supplement
    return true;
  }

  return false;
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
  bool haveCurrentPosition = false;
  double currentPenX = 0.0;
  double currentPenY = 0.0;
  int prevSpanLastGlyph = 0;  // Last glyph of previous span, for cross-span kerning.

  for (const auto& span : text.spans) {
    LayoutTextRun run;
    run.font = font;

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // Resolve per-span baseline-shift using actual font size for em units.
    // Positive CSS baseline-shift = shift up, which in SVG Y-down = subtract from Y.
    FontMetrics spanFontMetrics = params.fontMetrics;
    spanFontMetrics.fontSize = fontSizePx;
    const double spanBaselineShiftPx =
        span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);

    // If alignment-baseline is set on this span, it overrides the text-level dominant-baseline.
    double effectiveBaselineShift = baselineShift;
    if (span.alignmentBaseline != DominantBaseline::Auto) {
      effectiveBaselineShift = 0.0;
      int ascent = 0;
      int descent = 0;
      int lineGap = 0;
      stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
      switch (span.alignmentBaseline) {
        case DominantBaseline::Auto:
        case DominantBaseline::Alphabetic:
          break;
        case DominantBaseline::Middle:
        case DominantBaseline::Central:
          effectiveBaselineShift = static_cast<double>(ascent + descent) * 0.5 * scale;
          break;
        case DominantBaseline::Hanging:
          effectiveBaselineShift = static_cast<double>(ascent) * 0.8 * scale;
          break;
        case DominantBaseline::Mathematical:
          effectiveBaselineShift = static_cast<double>(ascent) * 0.5 * scale;
          break;
        case DominantBaseline::TextTop:
          effectiveBaselineShift = static_cast<double>(ascent) * scale;
          break;
        case DominantBaseline::TextBottom:
        case DominantBaseline::Ideographic:
          effectiveBaselineShift = static_cast<double>(descent) * scale;
          break;
      }
    }

    const bool vertical = isVertical(params.writingMode);
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

    // Decode each codepoint, look up glyph, accumulate advance + kerning.
    // charIdx tracks the addressable character index (grapheme cluster) for per-character
    // positioning attributes (x, y, dx, dy, rotate). Non-spacing characters and characters
    // following a ZWJ do not advance this index.
    size_t byteIdx = 0;
    // Suppress cross-span kerning when this span starts a new text chunk
    // (has explicit x or y position).
    int prevGlyph = (span.hasX || span.hasY) ? 0 : prevSpanLastGlyph;
    unsigned int charIdx = 0;
    bool firstCodepoint = true;
    bool prevWasZwj = false;
    while (byteIdx < spanText.size()) {
      const uint32_t codepoint = decodeUtf8(spanText, byteIdx);
      const bool combining = isNonSpacing(codepoint) || prevWasZwj;
      prevWasZwj = (codepoint == 0x200D);
      // Advance charIdx before non-combining characters (except the first), so combining marks
      // share the same index as their preceding base character.
      // UTF-16 surrogate pairs consume 2 indices for supplementary characters (U+10000+).
      if (!combining && !firstCodepoint) {
        charIdx += (codepoint >= 0x10000) ? 2 : 1;
      }
      firstCodepoint = false;
      const int glyphIndex = stbtt_FindGlyphIndex(info, static_cast<int>(codepoint));

      if (vertical) {
        // Vertical mode: primary advance is along Y, cross-axis is X.
        // Per-character absolute Y overrides the pen along the primary axis.
        const bool hasAbsoluteY =
            charIdx < span.yList.size() && span.yList[charIdx].has_value();
        if (hasAbsoluteY) {
          penY = span.yList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::Y);
        }

        // Per-character dy (primary axis).
        if (charIdx < span.dyList.size() && span.dyList[charIdx].has_value()) {
          penY += span.dyList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::Y);
        }

        // Per-character absolute X (cross-axis).
        if (charIdx < span.xList.size() && span.xList[charIdx].has_value()) {
          penX = span.xList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::X);
        }

        // Per-character dx (cross-axis).
        if (charIdx < span.dxList.size() && span.dxList[charIdx].has_value()) {
          penX += span.dxList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::X);
        }

        LayoutGlyph glyph;
        glyph.glyphIndex = glyphIndex;
        glyph.xPosition = penX;
        glyph.yPosition = penY;
        // Vertical advance: use fontSizePx (em-square height). stb_truetype lacks vmtx.
        glyph.yAdvance = fontSizePx;
        glyph.xAdvance = 0;

        // Rotation: non-CJK glyphs (codepoint < 0x2E80) get 90° CW rotation in vertical mode.
        double baseRotation = (codepoint < 0x2E80) ? 90.0 : 0.0;
        if (charIdx < span.rotateList.size()) {
          glyph.rotateDegrees = span.rotateList[charIdx] + baseRotation;
        } else if (!span.rotateList.empty()) {
          glyph.rotateDegrees = span.rotateList.back() + baseRotation;
        } else {
          glyph.rotateDegrees = span.rotateDegrees + baseRotation;
        }

        run.glyphs.push_back(glyph);

        penY += glyph.yAdvance;
        penY += params.letterSpacingPx;
        if (codepoint == 0x0020) {
          penY += params.wordSpacingPx;
        }
      } else {
        // Horizontal mode (existing path).
        // Per-character absolute X positioning overrides the pen.
        const bool hasAbsoluteX =
            charIdx < span.xList.size() && span.xList[charIdx].has_value();
        const bool hasAbsoluteY =
            charIdx < span.yList.size() && span.yList[charIdx].has_value();

        if (hasAbsoluteX) {
          penX = span.xList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::X);
        } else if (!hasAbsoluteY) {
          // Kern adjustment with previous glyph. Suppress kerning when a new text chunk
          // starts, which happens when the character has an absolute x OR y value.
          if (prevGlyph != 0 && glyphIndex != 0) {
            const int kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, glyphIndex);
            penX += static_cast<double>(kern) * scale;
          }
        }

        // Per-character dx.
        if (charIdx < span.dxList.size() && span.dxList[charIdx].has_value()) {
          penX += span.dxList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::X);
        }

        // Per-character absolute Y positioning.
        if (hasAbsoluteY) {
          penY = span.yList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                Lengthd::Extent::Y) +
                 baselineShift;
        }

        // Per-character dy.
        if (charIdx < span.dyList.size() && span.dyList[charIdx].has_value()) {
          penY += span.dyList[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::Y);
        }

        // Advance width.
        int advanceWidth = 0;
        int leftSideBearing = 0;
        stbtt_GetGlyphHMetrics(info, glyphIndex, &advanceWidth, &leftSideBearing);

        LayoutGlyph glyph;
        glyph.glyphIndex = glyphIndex;
        glyph.xPosition = penX;
        glyph.yPosition = penY;
        glyph.xAdvance = static_cast<double>(advanceWidth) * scale;

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
        if (codepoint == 0x0020) {
          penX += params.wordSpacingPx;
        }
      }

      prevGlyph = glyphIndex;
    }

    // If the span has path data, reposition glyphs along the path.
    if (span.pathSpline && !run.glyphs.empty()) {
      const auto& pathSpline = *span.pathSpline;
      const double startOffset = span.pathStartOffset;

      // Reposition each glyph at the midpoint of its advance along the path.
      double advanceAccum = 0.0;
      for (auto& g : run.glyphs) {
        // Sample the path at the center of this glyph's advance width.
        const double glyphMid = startOffset + advanceAccum + g.xAdvance * 0.5;
        const auto sample = pathSpline.pointAtArcLength(glyphMid);

        if (sample.valid) {
          // Shift the glyph origin back along the tangent by half its advance so the
          // glyph's visual center (not its left edge) sits at the path midpoint.
          const double halfAdv = g.xAdvance * 0.5;
          g.xPosition = sample.point.x - halfAdv * std::cos(sample.angle);
          g.yPosition = sample.point.y - halfAdv * std::sin(sample.angle);
          // Convert tangent angle to degrees and add the per-glyph rotation
          // (already set from per-character rotateList or span.rotateDegrees).
          g.rotateDegrees =
              sample.angle * MathConstants<double>::kRadToDeg + g.rotateDegrees;
        } else {
          // Past the end of the path — hide the glyph.
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

      // Skip textLength and text-anchor adjustments for path-based text.
      runs.push_back(std::move(run));
      continue;
    }

    // Apply textLength adjustment: stretch or compress glyph positions to fit the target length.
    if (params.textLength.has_value() && !run.glyphs.empty()) {
      const double targetLength = params.textLength->toPixels(
          params.viewBox, params.fontMetrics,
          vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);
      const double naturalLength = vertical ? (penY - runStartY) : (penX - runStartX);

      if (naturalLength > 0.0 && targetLength > 0.0) {
        if (params.lengthAdjust == LengthAdjust::Spacing) {
          const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
          const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
          for (size_t gi = 1; gi < run.glyphs.size(); ++gi) {
            if (vertical) {
              run.glyphs[gi].yPosition += extraPerGap * static_cast<double>(gi);
            } else {
              run.glyphs[gi].xPosition += extraPerGap * static_cast<double>(gi);
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

    // Apply text-anchor adjustment: shift all glyph positions along the inline axis.
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
    prevSpanLastGlyph = prevGlyph;
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
