#include "donner/svg/text/TextEngine.h"

#include <cmath>
#include <string>

#include "donner/base/MathUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/components/text/TextSystem.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/text/TextBackendSimple.h"
#include "donner/svg/text/TextEngineHelpers.h"
#ifdef DONNER_TEXT_FULL
#include "donner/svg/text/TextBackendFull.h"
#endif
#include "donner/svg/core/WritingMode.h"

namespace donner::svg {

namespace {

/// Decode a single UTF-8 codepoint, advancing \p i past the consumed bytes.
uint32_t decodeUtf8(const std::string_view str, size_t& i) {
  const auto [cp, length] = Utf8::NextCodepoint(str.substr(i));
  i += static_cast<size_t>(length);
  return static_cast<uint32_t>(cp);
}

/// Returns the first non-ASCII codepoint in \p str, or 0 if none exists.
uint32_t firstNonAsciiCodepoint(const std::string_view str) {
  for (size_t i = 0; i < str.size();) {
    const uint32_t cp = decodeUtf8(str, i);
    if (cp > 0x7F) {
      return cp;
    }
  }

  return 0;
}

/// Encode a single Unicode codepoint as UTF-8.
std::string encodeUtf8(uint32_t cp) {
  std::string result;
  Utf8::Append(static_cast<char32_t>(cp), std::back_inserter(result));
  return result;
}

/// Returns true if \p font shapes \p cp to a non-.notdef glyph.
bool fontSupportsCodepoint(const TextBackend& backend, FontHandle font, float fontSizePx,
                           uint32_t cp) {
  if (!font || cp == 0) {
    return false;
  }

  const std::string utf8 = encodeUtf8(cp);
  const auto shaped =
      backend.shapeRun(font, fontSizePx, utf8, 0, utf8.size(), false, FontVariant::Normal, false);
  return !shaped.glyphs.empty() && shaped.glyphs.front().glyphIndex != 0;
}

/// Finds a registered font that covers \p cp, preserving \p currentFont when it already does.
FontHandle findCoverageFallbackFont(const TextBackend& backend, FontManager& fontManager,
                                    FontHandle currentFont, float fontSizePx, uint32_t cp) {
  if (cp == 0 || fontSupportsCodepoint(backend, currentFont, fontSizePx, cp)) {
    return currentFont;
  }

  for (size_t i = 0; i < fontManager.numFaces(); ++i) {
    FontHandle candidate = fontManager.findFont(fontManager.faceFamilyName(i));
    if (candidate && candidate != currentFont &&
        fontSupportsCodepoint(backend, candidate, fontSizePx, cp)) {
      return candidate;
    }
  }

  return currentFont;
}

/**
 * Returns true if the codepoint is a non-spacing character that does not start a new
 * addressable character (grapheme cluster) for SVG per-character attributes.
 * Includes combining marks, zero-width joiners, and variation selectors.
 */
bool isNonSpacing(uint32_t cp) {
  // Combining marks (General Category M).
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

  // Zero-width joiners and format characters used in emoji/ligature sequences.
  if (cp == 0x200C ||  // Zero Width Non-Joiner
      cp == 0x200D ||  // Zero Width Joiner (emoji ZWJ sequences)
      cp == 0x034F) {  // Combining Grapheme Joiner
    return true;
  }

  // Variation selectors (emoji style selectors, ideographic variation sequences).
  if ((cp >= 0xFE00 && cp <= 0xFE0F) ||    // Variation Selectors (VS1-VS16)
      (cp >= 0xE0100 && cp <= 0xE01EF)) {  // Variation Selectors Supplement
    return true;
  }

  return false;
}

PathSpline transformPathSpline(const PathSpline& spline, const Transformd& transform) {
  PathSpline result;
  const auto& points = spline.points();

  for (const auto& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo:
        result.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case PathSpline::CommandType::LineTo:
        result.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case PathSpline::CommandType::CurveTo:
        result.curveTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]),
                       transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case PathSpline::CommandType::ClosePath: result.closePath(); break;
    }
  }

  return result;
}

Entity findTextRootEntity(EntityHandle handle) {
  Entity current = handle.entity();
  Registry& registry = *handle.registry();

  while (current != entt::null) {
    if (registry.any_of<components::TextRootComponent>(current)) {
      return current;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  return entt::null;
}

bool isDescendantOf(Registry& registry, Entity entity, Entity ancestor) {
  Entity current = entity;
  while (current != entt::null) {
    if (current == ancestor) {
      return true;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  return false;
}

TextLayoutParams buildTextLayoutParams(Registry& registry, EntityHandle handle,
                                       const components::ComputedStyleComponent& style,
                                       const components::TextComponent& textComp) {
  TextLayoutParams params;
  const auto& properties = style.properties.value();

  params.fontFamilies = properties.fontFamily.getRequired();
  params.fontSize = properties.fontSize.getRequired();
  params.viewBox = components::LayoutSystem().getViewBox(handle);

  const FontMetrics baseFontMetrics = FontMetrics::DefaultsWithFontSize(12.0);
  const double fontSizePx =
      params.fontSize.toPixels(params.viewBox, baseFontMetrics, Lengthd::Extent::Mixed);
  params.fontMetrics = FontMetrics::DefaultsWithFontSize(fontSizePx);

  params.textAnchor = properties.textAnchor.getRequired();
  params.dominantBaseline = properties.dominantBaseline.getRequired();
  params.writingMode = properties.writingMode.getRequired();
  params.letterSpacingPx = properties.letterSpacing.getRequired().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.wordSpacingPx = properties.wordSpacing.getRequired().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.textLength = textComp.textLength;
  params.lengthAdjust = textComp.lengthAdjust;
  return params;
}

void ResolvePerSpanLayoutStyles(Registry& registry, components::ComputedTextComponent& text,
                                const Boxd& viewBox, const FontMetrics& fontMetrics) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  for (auto& span : text.spans) {
    if (span.sourceEntity == entt::null) {
      continue;
    }

    auto* style = registry.try_get<components::ComputedStyleComponent>(span.sourceEntity);
    Entity styleEntity = span.sourceEntity;
    if ((!style || !style->properties) &&
        registry.all_of<donner::components::TreeComponent>(span.sourceEntity)) {
      const Entity parent =
          registry.get<donner::components::TreeComponent>(span.sourceEntity).parent();
      if (parent != entt::null) {
        style = registry.try_get<components::ComputedStyleComponent>(parent);
        styleEntity = parent;
      }
    }

    if (!style || !style->properties) {
      continue;
    }

    span.textAnchor = style->properties->textAnchor.getRequired();
    span.textDecoration = style->properties->textDecoration.getRequired();
    span.baselineShift = style->properties->baselineShift.getRequired();
    span.alignmentBaseline = style->properties->alignmentBaseline.getRequired();
    span.fontWeight = style->properties->fontWeight.getRequired();
    span.fontStyle = style->properties->fontStyle.getRequired();
    span.fontStretch = static_cast<FontStretch>(style->properties->fontStretch.getRequired());
    span.fontVariant = style->properties->fontVariant.getRequired();
    span.fontSize = style->properties->fontSize.getRequired();
    span.visibility = style->properties->visibility.getRequired();
    span.opacity = style->properties->opacity.getRequired();
    span.letterSpacingPx = style->properties->letterSpacing.getRequired().toPixels(
        viewBox, fontMetrics, Lengthd::Extent::X);
    span.wordSpacingPx = style->properties->wordSpacing.getRequired().toPixels(viewBox, fontMetrics,
                                                                               Lengthd::Extent::X);

    const bool isTextRoot = registry.any_of<components::TextRootComponent>(styleEntity);
    if (isTextRoot) {
      span.baselineShift = Lengthd(0, Lengthd::Unit::None);
    } else {
      if (span.baselineShift.unit == Lengthd::Unit::Em && span.baselineShift.value == -0.33) {
        span.baselineShiftKeyword = BSK::Sub;
      } else if (span.baselineShift.unit == Lengthd::Unit::Em && span.baselineShift.value == 0.4) {
        span.baselineShiftKeyword = BSK::Super;
      }

      Entity ancestor = styleEntity;
      while (registry.all_of<donner::components::TreeComponent>(ancestor)) {
        ancestor = registry.get<donner::components::TreeComponent>(ancestor).parent();
        if (ancestor == entt::null || registry.any_of<components::TextRootComponent>(ancestor)) {
          break;
        }

        auto* ancestorStyle = registry.try_get<components::ComputedStyleComponent>(ancestor);
        if (!ancestorStyle || !ancestorStyle->properties) {
          continue;
        }

        const Lengthd ancestorShift = ancestorStyle->properties->baselineShift.getRequired();
        const double ancestorFontSizePx =
            ancestorStyle->properties->fontSize.getRequired().toPixels(viewBox, fontMetrics,
                                                                       Lengthd::Extent::Mixed);
        BSK ancestorKeyword = BSK::Length;
        if (ancestorShift.unit == Lengthd::Unit::Em && ancestorShift.value == -0.33) {
          ancestorKeyword = BSK::Sub;
        } else if (ancestorShift.unit == Lengthd::Unit::Em && ancestorShift.value == 0.4) {
          ancestorKeyword = BSK::Super;
        }

        span.ancestorBaselineShifts.push_back({ancestorKeyword, ancestorShift, ancestorFontSizePx});
      }
    }

    if (auto* textComp = registry.try_get<components::TextComponent>(span.sourceEntity)) {
      if (textComp->textLength.has_value()) {
        span.textLength = textComp->textLength;
        span.lengthAdjust = textComp->lengthAdjust;
      }
    }
  }
}

}  // namespace

namespace text_engine_detail {

double computeBaselineShift(DominantBaseline baseline, const FontVMetrics& vm, float scale) {
  switch (baseline) {
    case DominantBaseline::Auto:
    case DominantBaseline::Alphabetic: return 0.0;
    case DominantBaseline::Middle:
    case DominantBaseline::Central:
      return static_cast<double>(vm.ascent + vm.descent) * 0.5 * scale;
    case DominantBaseline::Hanging:
      return static_cast<double>(vm.ascent) * 0.8 * scale;
    case DominantBaseline::Mathematical:
      return static_cast<double>(vm.ascent) * 0.5 * scale;
    case DominantBaseline::TextTop:
      return static_cast<double>(vm.ascent) * scale;
    case DominantBaseline::TextBottom:
    case DominantBaseline::Ideographic:
      return static_cast<double>(vm.descent) * scale;
  }
  return 0.0;
}

std::vector<ChunkRange> findChunkRanges(
    std::string_view spanText,
    const SmallVector<std::optional<Lengthd>, 1>& xList,
    const SmallVector<std::optional<Lengthd>, 1>& yList) {
  std::vector<ChunkRange> chunkRanges;
  size_t scanPos = 0;
  unsigned int scanCharIdx = 0;
  bool scanFirst = true;
  bool scanPrevWasZwj = false;
  size_t currentChunkStart = 0;

  while (scanPos < spanText.size()) {
    size_t byteStart = scanPos;
    uint32_t cp = decodeUtf8(spanText, scanPos);

    bool scanCombining = isNonSpacing(cp) || scanPrevWasZwj;
    scanPrevWasZwj = (cp == 0x200D);
    if (!scanCombining && !scanFirst) {
      scanCharIdx += (cp >= 0x10000) ? 2 : 1;
    }
    scanFirst = false;

    bool hasAbsX = scanCharIdx < xList.size() && xList[scanCharIdx].has_value();
    bool hasAbsY = scanCharIdx < yList.size() && yList[scanCharIdx].has_value();

    if ((hasAbsX || hasAbsY) && byteStart != currentChunkStart) {
      chunkRanges.push_back({currentChunkStart, byteStart});
      currentChunkStart = byteStart;
    }
  }
  chunkRanges.push_back({currentChunkStart, spanText.size()});
  return chunkRanges;
}

ByteIndexMappings buildByteIndexMappings(std::string_view spanText) {
  ByteIndexMappings result;
  result.byteToCharIdx.resize(spanText.size(), 0);
  result.byteToRawCpIdx.resize(spanText.size(), 0);

  unsigned int ci = 0;
  unsigned int rawCi = 0;
  bool mapFirst = true;
  bool mapPrevZwj = false;
  size_t bi = 0;

  while (bi < spanText.size()) {
    size_t startBi = bi;
    const uint32_t cp = decodeUtf8(spanText, bi);
    const bool nonSpacing = isNonSpacing(cp) || mapPrevZwj;
    mapPrevZwj = (cp == 0x200D);
    if (!nonSpacing && !mapFirst) {
      ci += (cp >= 0x10000) ? 2 : 1;
    }
    if (!mapFirst) {
      rawCi += (cp >= 0x10000) ? 2 : 1;
    }
    mapFirst = false;
    for (size_t j = startBi; j < bi && j < spanText.size(); ++j) {
      result.byteToCharIdx[j] = ci;
      result.byteToRawCpIdx[j] = rawCi;
    }
  }

  return result;
}

void applyTextLength(std::vector<TextRun>& runs,
                     const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents,
                     const TextLayoutParams& params, bool vertical, double currentPenX,
                     double currentPenY) {
  // Check if any span has per-span textLength.
  bool anySpanHasTextLength = false;
  for (const auto& span : text.spans) {
    if (span.textLength.has_value()) {
      anySpanHasTextLength = true;
      break;
    }
  }

  // ── Per-span textLength ───────────────────────────────────────────────
  if (anySpanHasTextLength) {
    for (size_t i = 0; i < runs.size() && i < text.spans.size(); ++i) {
      auto& run = runs[i];
      const auto& span = text.spans[i];
      if (!span.textLength.has_value() || run.glyphs.empty()) {
        continue;
      }

      const double runStartPos = vertical ? run.glyphs[0].yPosition : run.glyphs[0].xPosition;
      const auto& extent = runExtents[i];
      double naturalLength =
          vertical ? (extent.endY - extent.startY) : (extent.endX - extent.startX);

      if (naturalLength <= 0.0) {
        continue;
      }

      const double targetLength = span.textLength->toPixels(
          params.viewBox, params.fontMetrics, vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);

      if (targetLength <= 0.0) {
        continue;
      }

      if (span.lengthAdjust == LengthAdjust::Spacing) {
        const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
        const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
        for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
          if (vertical) {
            run.glyphs[gi].yPosition += extraPerGap * static_cast<double>(gi);
          } else {
            run.glyphs[gi].xPosition += extraPerGap * static_cast<double>(gi);
          }
        }
      } else {
        const double scaleFactor = targetLength / naturalLength;
        for (auto& g : run.glyphs) {
          if (vertical) {
            g.yPosition = runStartPos + (g.yPosition - runStartPos) * scaleFactor;
            g.yAdvance *= scaleFactor;
          } else {
            g.xPosition = runStartPos + (g.xPosition - runStartPos) * scaleFactor;
            g.xAdvance *= scaleFactor;
          }
        }
      }
    }
  }

  // ── Global textLength ────────────���────────────────────────────────────
  double globalStartX = currentPenX;
  double globalStartY = currentPenY;
  for (const auto& r : runs) {
    if (!r.glyphs.empty()) {
      globalStartX = r.glyphs[0].xPosition;
      globalStartY = r.glyphs[0].yPosition;
      break;
    }
  }

  const double globalNaturalLength =
      vertical ? (currentPenY - globalStartY) : (currentPenX - globalStartX);

  if (!anySpanHasTextLength && params.textLength.has_value() && globalNaturalLength > 0.0) {
    const double targetLength = params.textLength->toPixels(
        params.viewBox, params.fontMetrics, vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);

    if (targetLength > 0.0) {
      size_t totalGlyphs = 0;
      for (const auto& r : runs) {
        totalGlyphs += r.glyphs.size();
      }

      if (params.lengthAdjust == LengthAdjust::Spacing) {
        const size_t numGaps = totalGlyphs > 1 ? totalGlyphs - 1 : 1;
        const double extraPerGap =
            (targetLength - globalNaturalLength) / static_cast<double>(numGaps);
        size_t glyphIdx = 0;
        for (auto& r : runs) {
          for (auto& g : r.glyphs) {
            if (vertical) {
              g.yPosition += extraPerGap * static_cast<double>(glyphIdx);
            } else {
              g.xPosition += extraPerGap * static_cast<double>(glyphIdx);
            }
            ++glyphIdx;
          }
        }
      } else {
        const double scaleFactor = targetLength / globalNaturalLength;
        for (auto& r : runs) {
          for (auto& g : r.glyphs) {
            if (vertical) {
              g.yPosition = globalStartY + (g.yPosition - globalStartY) * scaleFactor;
              g.yAdvance *= scaleFactor;
            } else {
              g.xPosition = globalStartX + (g.xPosition - globalStartX) * scaleFactor;
              g.xAdvance *= scaleFactor;
            }
          }
        }
      }
    }
  }
}

/// Fix up chunk text-anchors and apply per-chunk text-anchor adjustment.
void applyTextAnchor(std::vector<TextRun>& runs,
                     std::vector<ChunkBoundary>& chunkBoundaries,
                     const components::ComputedTextComponent& text, bool vertical) {
  // For each chunk, use the text-anchor from the first span that has actual glyph content.
  for (auto& chunk : chunkBoundaries) {
    for (size_t ri = chunk.runIndex; ri < runs.size(); ++ri) {
      const size_t gStart = (ri == chunk.runIndex) ? chunk.glyphIndex : 0;
      if (gStart < runs[ri].glyphs.size()) {
        chunk.textAnchor = text.spans[ri].textAnchor;
        break;
      }
    }
  }

  // Apply text-anchor adjustment per text chunk.
  for (size_t ci = 0; ci < chunkBoundaries.size(); ++ci) {
    const auto& chunk = chunkBoundaries[ci];
    if (chunk.textAnchor == TextAnchor::Start) {
      continue;
    }

    size_t endRunIdx = runs.size();
    size_t endGlyphIdx = 0;
    if (ci + 1 < chunkBoundaries.size()) {
      endRunIdx = chunkBoundaries[ci + 1].runIndex;
      endGlyphIdx = chunkBoundaries[ci + 1].glyphIndex;
    }

    double chunkStartPos = 0.0;
    double chunkEndPos = 0.0;
    bool foundFirst = false;
    for (size_t ri = chunk.runIndex; ri <= std::min(endRunIdx, runs.size() - 1); ++ri) {
      const size_t gStart = (ri == chunk.runIndex) ? chunk.glyphIndex : 0;
      const size_t gEnd = (ri == endRunIdx) ? endGlyphIdx : runs[ri].glyphs.size();
      for (size_t gi = gStart; gi < gEnd; ++gi) {
        const auto& g = runs[ri].glyphs[gi];
        const double pos = vertical ? g.yPosition : g.xPosition;
        const double adv = vertical ? g.yAdvance : g.xAdvance;
        if (!foundFirst) {
          chunkStartPos = pos;
          chunkEndPos = pos + adv;
          foundFirst = true;
        } else {
          chunkEndPos = pos + adv;
        }
      }
    }

    if (!foundFirst) {
      continue;
    }

    const double chunkLength = chunkEndPos - chunkStartPos;
    double shift = 0.0;
    if (chunk.textAnchor == TextAnchor::Middle) {
      shift = -chunkLength / 2.0;
    } else if (chunk.textAnchor == TextAnchor::End) {
      shift = -chunkLength;
    }

    for (size_t ri = chunk.runIndex; ri <= std::min(endRunIdx, runs.size() - 1); ++ri) {
      const size_t gStart = (ri == chunk.runIndex) ? chunk.glyphIndex : 0;
      const size_t gEnd = (ri == endRunIdx) ? endGlyphIdx : runs[ri].glyphs.size();
      for (size_t gi = gStart; gi < gEnd; ++gi) {
        if (vertical) {
          runs[ri].glyphs[gi].yPosition += shift;
        } else {
          runs[ri].glyphs[gi].xPosition += shift;
        }
      }
    }
  }
}

/// Compute per-span baseline-shift in pixels, including OS/2 sub/super metrics
/// and ancestor baseline-shift accumulation.
double computeSpanBaselineShiftPx(
    const TextBackend& backend,
    const components::ComputedTextComponent::TextSpan& span,
    FontHandle spanFont, float spanScale, const TextLayoutParams& params) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  FontMetrics spanFontMetrics = params.fontMetrics;
  const float spanFontSizePx =
      span.fontSize.value != 0.0
          ? static_cast<float>(span.fontSize.toPixels(params.viewBox, params.fontMetrics,
                                                       Lengthd::Extent::Mixed))
          : static_cast<float>(
                params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
  spanFontMetrics.fontSize = spanFontSizePx;

  double spanBaselineShiftPx;
  if (span.baselineShiftKeyword == BSK::Sub || span.baselineShiftKeyword == BSK::Super) {
    const auto subSuper = backend.subSuperMetrics(spanFont);
    if (subSuper.has_value()) {
      if (span.baselineShiftKeyword == BSK::Sub) {
        spanBaselineShiftPx = -static_cast<double>(subSuper->subscriptYOffset) * spanScale;
      } else {
        spanBaselineShiftPx = static_cast<double>(subSuper->superscriptYOffset) * spanScale;
      }
    } else {
      spanBaselineShiftPx =
          span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
    }
  } else {
    spanBaselineShiftPx =
        span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
  }

  // Resolve ancestor baseline-shifts.
  {
    const auto subSuper = backend.subSuperMetrics(spanFont);
    for (const auto& ancestor : span.ancestorBaselineShifts) {
      if (subSuper.has_value() && ancestor.keyword == BSK::Sub) {
        const float ancestorScale =
            backend.scaleForEmToPixels(spanFont, static_cast<float>(ancestor.fontSizePx));
        spanBaselineShiftPx += -static_cast<double>(subSuper->subscriptYOffset) * ancestorScale;
      } else if (subSuper.has_value() && ancestor.keyword == BSK::Super) {
        const float ancestorScale =
            backend.scaleForEmToPixels(spanFont, static_cast<float>(ancestor.fontSizePx));
        spanBaselineShiftPx += static_cast<double>(subSuper->superscriptYOffset) * ancestorScale;
      } else {
        FontMetrics ancestorFm = params.fontMetrics;
        ancestorFm.fontSize = ancestor.fontSizePx;
        spanBaselineShiftPx +=
            ancestor.shift.toPixels(params.viewBox, ancestorFm, Lengthd::Extent::Y);
      }
    }
  }

  return spanBaselineShiftPx;
}

}  // namespace text_engine_detail

using namespace text_engine_detail;  // NOLINT(google-build-using-namespace)

namespace {

void addBox(Boxd& accum, bool& initialized, const Boxd& box) {
  if (box.isEmpty()) {
    return;
  }

  if (!initialized) {
    accum = box;
    initialized = true;
  } else {
    accum.addBox(box);
  }
}

std::vector<const components::ComputedTextGeometryComponent::CharacterGeometry*> filteredCharacters(
    Registry& registry, EntityHandle handle, const components::ComputedTextGeometryComponent& cache) {
  std::vector<const components::ComputedTextGeometryComponent::CharacterGeometry*> result;
  for (const auto& character : cache.characters) {
    if (character.rendered && isDescendantOf(registry, character.sourceEntity, handle.entity())) {
      result.push_back(&character);
    }
  }
  return result;
}

}  // namespace

TextEngine::TextEngine(FontManager& fontManager, Registry& registry)
    : fontManager_(fontManager), registry_(registry) {
#ifdef DONNER_TEXT_FULL
  backend_ = std::make_unique<TextBackendFull>(fontManager_, registry_);
#else
  backend_ = std::make_unique<TextBackendSimple>(fontManager_, registry_);
#endif
}

TextEngine::TextEngine(FontManager& fontManager, Registry& registry,
                       std::unique_ptr<TextBackend> backend)
    : fontManager_(fontManager), registry_(registry), backend_(std::move(backend)) {}

TextEngine::~TextEngine() = default;

void TextEngine::prepareForElement(EntityHandle handle, std::vector<ParseError>* outWarnings) {
  UTILS_RELEASE_ASSERT(handle.registry() == &registry_);
  const Entity textRootEntity = findTextRootEntity(handle);
  if (textRootEntity == entt::null) {
    return;
  }

  auto* resourceManager = registry_.ctx().find<components::ResourceManagerContext>();
  if (!resourceManager) {
    return;
  }

  if (resourceManager->fontFaces().empty()) {
    for (auto view = registry_.view<components::StylesheetComponent>(); auto entity : view) {
      resourceManager->addFontFaces(
          view.get<components::StylesheetComponent>(entity).stylesheet.fontFaces());
    }
  }
  resourceManager->loadResources(outWarnings);

  auto& fontManager = registry_.ctx().contains<FontManager>()
                          ? registry_.ctx().get<FontManager>()
                          : registry_.ctx().emplace<FontManager>(registry_);
  if (!registry_.ctx().contains<TextEngine>()) {
    registry_.ctx().emplace<TextEngine>(fontManager, registry_);
  }
  addFontFaces(resourceManager->fontFaces());

  components::StyleSystem styleSystem;
  const EntityHandle textRootHandle(registry_, textRootEntity);
  styleSystem.computeStyle(textRootHandle, outWarnings);
  donner::components::ForAllChildrenRecursive(
      textRootHandle, [&](EntityHandle child) { styleSystem.computeStyle(child, outWarnings); });

  components::TextSystem().instantiateComputedComponent(textRootHandle, outWarnings);
}

void TextEngine::resolvePerSpanLayoutStyles(EntityHandle textRootHandle,
                                            components::ComputedTextComponent& text) const {
  const auto* textComp = registry_.try_get<components::TextComponent>(textRootHandle.entity());
  const auto* style =
      registry_.try_get<components::ComputedStyleComponent>(textRootHandle.entity());
  if (!textComp || !style || !style->properties) {
    return;
  }

  const TextLayoutParams params =
      buildTextLayoutParams(registry_, textRootHandle, *style, *textComp);
  ResolvePerSpanLayoutStyles(registry_, text, params.viewBox, params.fontMetrics);
}

void TextEngine::addFontFace(const css::FontFace& face) {
  fontManager_.addFontFace(face);
  ++registeredFontFaceCount_;
}

void TextEngine::addFontFaces(std::span<const css::FontFace> faces) {
  if (faces.size() <= registeredFontFaceCount_) {
    return;
  }

  for (size_t i = registeredFontFaceCount_; i < faces.size(); ++i) {
    fontManager_.addFontFace(faces[i]);
  }
  registeredFontFaceCount_ = faces.size();
}

std::vector<TextRun> TextEngine::layout(const components::ComputedTextComponent& text,
                                        const TextLayoutParams& params) {
  // ── Resolve base font ─────────────────────────────────────────────────────────
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

  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // ── Compute dominant-baseline shift ───────────────────────────────────────────
  const float scale = backend_->scaleForPixelHeight(font, fontSizePx);
  double baselineShift = 0.0;
  if (params.dominantBaseline != DominantBaseline::Auto &&
      params.dominantBaseline != DominantBaseline::Alphabetic) {
    baselineShift = computeBaselineShift(params.dominantBaseline, backend_->fontVMetrics(font), scale);
  }

  // ── Layout state ──────────────────────────────────────────────────────────────
  std::vector<TextRun> runs;
  bool haveCurrentPosition = false;
  double currentPenX = 0.0;
  double currentPenY = 0.0;
  double prevDefaultY = 0.0;  // Previous span's baseline-shift offset, for undo/redo between spans.
  uint32_t prevSpanLastCodepoint = 0;  // Last codepoint of previous span, for cross-span kerning.
  FontHandle prevSpanFont;
  float prevSpanFontSizePx = 0.0f;

  std::vector<ChunkBoundary> chunkBoundaries;
  std::vector<RunPenExtent> runExtents;

  // ── Per-span layout loop ──────────────────────────────────────────────────────
  for (const auto& span : text.spans) {
    TextRun run;

    // Hidden spans (display:none) are not rendered. Push empty run.
    if (span.hidden) {
      runExtents.push_back({0.0, 0.0, 0.0, 0.0});
      runs.push_back(std::move(run));
      continue;
    }

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // ── Per-span font resolution ────────────────────────────────────────────────
    FontHandle spanFont = font;
    if (span.fontWeight != 400 || span.fontStyle != FontStyle::Normal ||
        span.fontStretch != FontStretch::Normal) {
      for (const auto& family : params.fontFamilies) {
        FontHandle candidate =
            fontManager_.findFont(family, span.fontWeight, static_cast<int>(span.fontStyle),
                                  static_cast<int>(span.fontStretch));
        if (candidate) {
          spanFont = candidate;
          break;
        }
      }
    }

    // Per-span font size: use the span's fontSize if set, otherwise the text element's.
    const float spanFontSizePx =
        span.fontSize.value != 0.0
            ? static_cast<float>(span.fontSize.toPixels(params.viewBox, params.fontMetrics,
                                                        Lengthd::Extent::Mixed))
            : fontSizePx;

    const uint32_t spanTestCodepoint = firstNonAsciiCodepoint(spanText);
    spanFont = findCoverageFallbackFont(*backend_, fontManager_, spanFont, spanFontSizePx,
                                        spanTestCodepoint);
    run.font = spanFont;

    // Note: bitmap-only fonts (e.g., color emoji) are valid for the full backend.
    // The simple backend can't handle them but produces empty shapeRun results,
    // which the renderer gracefully skips.

    const float spanScale = backend_->scaleForEmToPixels(spanFont, spanFontSizePx);

    // ── Per-span baseline-shift ─────────────────────────────────────────────────
    const double spanBaselineShiftPx =
        computeSpanBaselineShiftPx(*backend_, span, spanFont, spanScale, params);

    // ── Alignment-baseline override ─────────────────────────────────────────────
    double effectiveBaselineShift = baselineShift;
    if (span.alignmentBaseline != DominantBaseline::Auto) {
      effectiveBaselineShift =
          computeBaselineShift(span.alignmentBaseline, backend_->fontVMetrics(spanFont), spanScale);
    }

    // ── Initial pen position ────────────────────────────────────────────────────
    const bool vertical = isVertical(params.writingMode);
    double penX = haveCurrentPosition ? currentPenX : 0.0;
    const double defaultY = effectiveBaselineShift - spanBaselineShiftPx;
    // Always apply this span's baseline-shift. When transitioning from a previous span, undo
    // the previous span's shift and apply the current span's shift so that baseline-shift is
    // correctly scoped to each span (sub/super/length shifts don't leak to siblings).
    double penY = haveCurrentPosition ? (currentPenY - prevDefaultY + defaultY) : defaultY;

    // Apply span-start positioning from xList[0]/yList[0], then clear index 0
    // so the glyph loop doesn't double-apply.
    // Copy positioning lists so span-start can consume index 0 without
    // double-applying in the glyph loop.
    SmallVector<std::optional<Lengthd>, 1> xListLocal = span.xList;
    SmallVector<std::optional<Lengthd>, 1> yListLocal = span.yList;
    SmallVector<std::optional<Lengthd>, 1> dxListLocal = span.dxList;
    SmallVector<std::optional<Lengthd>, 1> dyListLocal = span.dyList;
    if (span.startsNewChunk || !haveCurrentPosition) {
      if (!xListLocal.empty() && xListLocal[0].has_value()) {
        penX = xListLocal[0]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
        xListLocal[0].reset();
      }
      if (!dxListLocal.empty() && dxListLocal[0].has_value()) {
        penX += dxListLocal[0]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
        dxListLocal[0].reset();
      }
      if (!yListLocal.empty() && yListLocal[0].has_value()) {
        penY = yListLocal[0]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y) +
               defaultY;
        yListLocal[0].reset();
      }
      if (!dyListLocal.empty() && dyListLocal[0].has_value()) {
        penY += dyListLocal[0]->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y);
        dyListLocal[0].reset();
      }
    }

    // Record chunk boundary at span start if this span starts a new chunk.
    // The text-anchor may be updated later if this span is empty (the first
    // non-empty span's text-anchor is used for the chunk).
    if (span.startsNewChunk || !haveCurrentPosition) {
      chunkBoundaries.push_back({runs.size(), 0, span.textAnchor});
    }

    // For empty spans, span-start already applied positioning — just propagate.
    if (spanText.empty()) {
      currentPenX = penX;
      currentPenY = penY;
      prevDefaultY = defaultY;
      haveCurrentPosition = true;
      runExtents.push_back({penX, penY, penX, penY});
      runs.push_back(std::move(run));
      continue;
    }

    // Capture pen position at the start of glyph layout for per-span textLength.
    const double runPenStartX = penX;
    const double runPenStartY = penY;

    // ── Pre-scan span text to find chunk byte ranges ─────────────────────────────
    const std::vector<ChunkRange> chunkRanges = findChunkRanges(spanText, xListLocal, yListLocal);

    // ── Build byte→index mappings for the span text ──────────────────────────────
    const ByteIndexMappings indexMappings = buildByteIndexMappings(spanText);
    const auto& byteToCharIdx = indexMappings.byteToCharIdx;
    const auto& byteToRawCpIdx = indexMappings.byteToRawCpIdx;

    // ── Shape each chunk and iterate its glyphs ─────────────────────────────────
    uint32_t lastCodepoint = 0;

    // Track last codepoint per chunk for cross-chunk kerning.
    uint32_t prevChunkLastCodepoint = prevSpanLastCodepoint;
    FontHandle prevChunkFont = prevSpanFont;
    float prevChunkFontSizePx = prevSpanFontSizePx;

    for (size_t ci = 0; ci < chunkRanges.size(); ++ci) {
      const auto& chunk = chunkRanges[ci];
      const auto shaped =
          backend_->shapeRun(spanFont, spanFontSizePx, spanText, chunk.byteStart,
                             chunk.byteEnd - chunk.byteStart, vertical, span.fontVariant, false);

      // ── RTL Y-override for multi-glyph chunks ─────────────────────────────────
      // When a multi-glyph RTL chunk starts because of an absolute y position on the
      // first DOM character, all glyphs in the chunk should share that y. HarfBuzz
      // returns RTL glyphs in visual order (last DOM char first), so the engine would
      // otherwise apply the explicit y to the wrong glyph. Pre-compute the override.
      std::optional<double> chunkYOverride;
      const bool isRTLChunk =
          shaped.glyphs.size() > 1 && shaped.glyphs.front().cluster > shaped.glyphs.back().cluster;
      if (isRTLChunk && ci > 0) {
        // The chunk started at chunk.byteStart — check if that byte's charIdx has absolute y.
        const unsigned int firstCharIdx =
            chunk.byteStart < byteToCharIdx.size() ? byteToCharIdx[chunk.byteStart] : 0;
        if (firstCharIdx < yListLocal.size() && yListLocal[firstCharIdx].has_value()) {
          chunkYOverride = yListLocal[firstCharIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                              Lengthd::Extent::Y) +
                           defaultY;
        }
      }

      // ── Cross-chunk / cross-span kerning ──────────────────────────────────────
      double crossKern = 0.0;
      bool appliedCrossKern = false;
      if (ci > 0 && prevChunkLastCodepoint != 0 && !shaped.glyphs.empty()) {
        // Cross-chunk kerning (between chunks within the same span).
        size_t firstByteIdx = chunk.byteStart;
        const uint32_t firstCp = decodeUtf8(spanText, firstByteIdx);
        crossKern =
            backend_->crossSpanKern(prevChunkFont, prevChunkFontSizePx, spanFont, spanFontSizePx,
                                    prevChunkLastCodepoint, firstCp, vertical);
        appliedCrossKern = true;
      } else if (ci == 0 && !span.startsNewChunk && prevSpanLastCodepoint != 0 &&
                 !shaped.glyphs.empty()) {
        // Cross-span kerning for the first chunk.
        size_t firstByteIdx = chunk.byteStart;
        const uint32_t firstCp = decodeUtf8(spanText, firstByteIdx);
        crossKern =
            backend_->crossSpanKern(prevSpanFont, prevSpanFontSizePx, spanFont, spanFontSizePx,
                                    prevSpanLastCodepoint, firstCp, vertical);
        appliedCrossKern = true;
      }

      // ── Iterate shaped glyphs within this chunk ───────────────────────────────
      // When per-char positioning exists, the backend returns glyphs in logical (DOM)
      // order via forceLogicalOrder. Otherwise, glyphs are in visual order.
      for (size_t gi = 0; gi < shaped.glyphs.size(); ++gi) {
        const auto& sg = shaped.glyphs[gi];

        // Decode codepoint from the cluster byte offset into spanText.
        size_t clusterPos = sg.cluster;
        const uint32_t codepoint = decodeUtf8(spanText, clusterPos);

        // Look up per-character indices from the byte→index mappings.
        const unsigned int charIdx =
            sg.cluster < byteToCharIdx.size() ? byteToCharIdx[sg.cluster] : 0;
        const unsigned int rawCpIdx =
            sg.cluster < byteToRawCpIdx.size() ? byteToRawCpIdx[sg.cluster] : 0;

        if (vertical) {
          // ── Vertical mode ─────────────────────────────────────────────────────
          // Primary advance is along Y, cross-axis is X.
          const bool hasAbsoluteY = charIdx < yListLocal.size() && yListLocal[charIdx].has_value();
          const bool hasAbsoluteX_v =
              charIdx < xListLocal.size() && xListLocal[charIdx].has_value();
          if ((hasAbsoluteX_v || hasAbsoluteY) && !(ci == 0 && gi == 0)) {
            chunkBoundaries.push_back({runs.size(), run.glyphs.size(), span.textAnchor});
          }

          // Per-character absolute Y (primary axis).
          if (hasAbsoluteY) {
            penY = yListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::Y);
          }

          // Per-character dy (primary axis).
          if (charIdx < dyListLocal.size() && dyListLocal[charIdx].has_value()) {
            penY += dyListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                   Lengthd::Extent::Y);
          }

          // Per-character absolute X (cross-axis).
          if (charIdx < xListLocal.size() && xListLocal[charIdx].has_value()) {
            penX = xListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::X);
          }

          // Per-character dx (cross-axis).
          if (charIdx < dxListLocal.size() && dxListLocal[charIdx].has_value()) {
            penX += dxListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                   Lengthd::Extent::X);
          }

          // Apply kerning: cross-chunk/cross-span for first glyph, within-chunk for subsequent.
          // Suppress when the glyph has absolute positioning.
          if (!hasAbsoluteX_v && !hasAbsoluteY) {
            if (gi == 0 && appliedCrossKern) {
              penY += crossKern;
            } else if (gi != 0) {
              penY += sg.yKern;
            }
          }

          TextGlyph glyph;
          glyph.glyphIndex = sg.glyphIndex;
          glyph.xPosition = penX;
          glyph.yPosition = penY;
          glyph.xAdvance = 0;
          glyph.fontSizeScale = sg.fontSizeScale;
          glyph.cluster = sg.cluster;

          // Vertical mode glyph handling: non-CJK glyphs get 90 degrees CW rotation.
          if (codepoint < 0x2E80) {
            // Sideways Latin glyph: rotated 90 degrees CW per SVG spec.
            // The shapeRun always returns horizontal advances (LTR shaping).
            // For sideways glyphs, use xAdvance as the vertical advance.
            glyph.yAdvance = sg.xAdvance > 0 ? sg.xAdvance : sg.yAdvance;

            // Apply kerning to vertical pen (sideways glyphs use horizontal metrics).
            // The shapeRun already includes within-span kerning in xAdvance/yAdvance,
            // but we need to handle the case where the absolute Y resets the pen.
            // Note: within-span kerning is already baked into the shaped advances.

            // Center on the central baseline: shift X so the midpoint between the scaled
            // ascender and descender aligns with the text position. Use pixel-height scaling
            // (ascent-to-descent = fontSize) so the center is proportionally correct.
            const FontVMetrics vm = backend_->fontVMetrics(spanFont);
            // Compute ascent-descent-based scale: pixelHeight / (ascent - descent).
            // This differs from em-based scaling used elsewhere.
            const double phScale = (vm.ascent != vm.descent) ? static_cast<double>(spanFontSizePx) /
                                                                   (vm.ascent - vm.descent)
                                                             : 0.0;
            const double centralBaselineOffset =
                (static_cast<double>(vm.ascent) + static_cast<double>(vm.descent)) * phScale / 2.0;
            glyph.xPosition -= centralBaselineOffset;

            double baseRotation = 90.0;
            if (rawCpIdx < span.rotateList.size()) {
              glyph.rotateDegrees = span.rotateList[rawCpIdx] + baseRotation;
            } else if (!span.rotateList.empty()) {
              glyph.rotateDegrees = span.rotateList.back() + baseRotation;
            } else {
              glyph.rotateDegrees = baseRotation;
            }
          } else {
            // Upright CJK: use backend-provided vertical offsets and advance.
            glyph.yAdvance = sg.yAdvance > 0 ? sg.yAdvance : spanFontSizePx;
            glyph.xPosition = penX + sg.xOffset;
            glyph.yPosition = penY + sg.yOffset;

            if (rawCpIdx < span.rotateList.size()) {
              glyph.rotateDegrees = span.rotateList[rawCpIdx];
            } else if (!span.rotateList.empty()) {
              glyph.rotateDegrees = span.rotateList.back();
            }
          }

          run.glyphs.push_back(glyph);

          penY += glyph.yAdvance;
          // Letter-spacing: suppress for cursive scripts.
          if (!backend_->isCursive(codepoint)) {
            penY += span.letterSpacingPx;
          }
          // Word-spacing after U+0020 (space).
          if (codepoint == 0x0020) {
            penY += span.wordSpacingPx;
          }
        } else {
          // ── Horizontal mode ───────────────────────────────────────────────────
          const bool hasAbsoluteX = charIdx < xListLocal.size() && xListLocal[charIdx].has_value();
          const bool hasAbsoluteY = charIdx < yListLocal.size() && yListLocal[charIdx].has_value();

          // A within-span absolute x or y starts a new text chunk (unless it's the
          // first glyph of the span that already started a chunk via startsNewChunk).
          if ((hasAbsoluteX || hasAbsoluteY) && !(ci == 0 && gi == 0)) {
            chunkBoundaries.push_back({runs.size(), run.glyphs.size(), span.textAnchor});
          }

          // Apply kerning: cross-chunk/cross-span for first glyph, within-chunk for subsequent.
          // Suppress when the glyph has absolute positioning (per SVG spec).
          if (!hasAbsoluteX && !hasAbsoluteY) {
            if (gi == 0 && appliedCrossKern) {
              penX += crossKern;
            } else if (gi != 0) {
              penX += sg.xKern;
            }
          }

          if (hasAbsoluteX) {
            penX = xListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::X);
          }

          // Per-character dx.
          if (charIdx < dxListLocal.size() && dxListLocal[charIdx].has_value()) {
            penX += dxListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                   Lengthd::Extent::X);
          }

          // Per-character absolute Y positioning.
          // For multi-glyph RTL chunks, use the chunk Y override so all glyphs share
          // the same baseline (matching old TextShaper's chunkYOverrides behavior).
          if (chunkYOverride.has_value()) {
            penY = *chunkYOverride;
          } else if (hasAbsoluteY) {
            penY = yListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                 Lengthd::Extent::Y) +
                   defaultY;
          }

          // Per-character dy.
          if (charIdx < dyListLocal.size() && dyListLocal[charIdx].has_value()) {
            penY += dyListLocal[charIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                   Lengthd::Extent::Y);
          }

          TextGlyph glyph;
          glyph.glyphIndex = sg.glyphIndex;
          glyph.xPosition = penX + sg.xOffset;
          glyph.yPosition = penY + sg.yOffset;
          glyph.xAdvance = sg.xAdvance;
          glyph.yAdvance = sg.yAdvance;
          glyph.fontSizeScale = sg.fontSizeScale;
          glyph.cluster = sg.cluster;

          // Per-character rotation (last value repeats per SVG spec).
          if (rawCpIdx < span.rotateList.size()) {
            glyph.rotateDegrees = span.rotateList[rawCpIdx];
          } else if (!span.rotateList.empty()) {
            glyph.rotateDegrees = span.rotateList.back();
          }

          // Rotate combining mark offsets around the base glyph so the cluster rotates together.
          if (glyph.rotateDegrees != 0.0 && !run.glyphs.empty() && glyph.xAdvance == 0.0) {
            const auto& base = run.glyphs.back();
            if (base.xAdvance != 0.0) {
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

          // Supplementary characters consume a trailing UTF-16 code unit. Apply its coordinate
          // values to the next glyph to preserve SVG's UTF-16 indexed positioning semantics.
          if (codepoint >= 0x10000) {
            const unsigned int lowIdx = charIdx + 1;
            if (lowIdx < yListLocal.size() && yListLocal[lowIdx].has_value()) {
              penY = yListLocal[lowIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::Y) +
                     defaultY;
            }
            if (lowIdx < xListLocal.size() && xListLocal[lowIdx].has_value()) {
              penX = xListLocal[lowIdx]->toPixels(params.viewBox, params.fontMetrics,
                                                  Lengthd::Extent::X);
            }
          }

          penX += sg.xAdvance;

          // CSS letter-spacing: extra space after every character, suppressed for cursive scripts.
          if (!backend_->isCursive(codepoint)) {
            penX += span.letterSpacingPx;
          }
          // CSS word-spacing: extra space after U+0020 (space) characters.
          if (codepoint == 0x0020) {
            penX += span.wordSpacingPx;
          }
        }

        lastCodepoint = codepoint;
      }

      // Track last codepoint of this chunk for cross-chunk kerning in the next chunk.
      if (!shaped.glyphs.empty()) {
        size_t lastCluster = shaped.glyphs.back().cluster;
        prevChunkLastCodepoint = decodeUtf8(spanText, lastCluster);
        prevChunkFont = spanFont;
        prevChunkFontSizePx = spanFontSizePx;
      }
    }

    // ── Text-on-path ──────────────────────────────────────────────────────────
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
          // (already set from per-character rotateList).
          g.rotateDegrees = sample.angle * MathConstants<double>::kRadToDeg + g.rotateDegrees;
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
      prevDefaultY = 0.0;  // Path sets absolute position; no shift to undo.

      // Hidden/collapsed spans on a path still advance along the path but are not drawn.
      if (span.visibility != Visibility::Visible) {
        run.glyphs.clear();
      }

      // Skip textLength and text-anchor adjustments for path-based text.
      runExtents.push_back({runPenStartX, runPenStartY, penX, penY});
      runs.push_back(std::move(run));
      continue;
    }

    // ── Update cross-span state ─────────────────────────────────────────────────
    currentPenX = penX;
    currentPenY = penY;
    prevDefaultY = defaultY;
    haveCurrentPosition = true;
    prevSpanLastCodepoint = lastCodepoint;
    prevSpanFont = spanFont;
    prevSpanFontSizePx = spanFontSizePx;

    // Hidden/collapsed spans participate in layout (pen advances above) but their glyphs
    // are not rendered. Clear the glyph list so the renderer skips this run.
    if (span.visibility != Visibility::Visible) {
      run.glyphs.clear();
    }

    runExtents.push_back({runPenStartX, runPenStartY, penX, penY});
    runs.push_back(std::move(run));
  }

  // ── textLength and text-anchor adjustments ────────────────────────────────────
  if (!runs.empty()) {
    const bool vertical = isVertical(params.writingMode);
    applyTextLength(runs, text, runExtents, params, vertical, currentPenX, currentPenY);
    applyTextAnchor(runs, chunkBoundaries, text, vertical);
  }

  return runs;
}

FontVMetrics TextEngine::fontVMetrics(FontHandle font) const {
  return backend_->fontVMetrics(font);
}

float TextEngine::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  return backend_->scaleForPixelHeight(font, pixelHeight);
}

float TextEngine::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  return backend_->scaleForEmToPixels(font, pixelHeight);
}

std::optional<UnderlineMetrics> TextEngine::underlineMetrics(FontHandle font) const {
  return backend_->underlineMetrics(font);
}

std::optional<SubSuperMetrics> TextEngine::subSuperMetrics(FontHandle font) const {
  return backend_->subSuperMetrics(font);
}

PathSpline TextEngine::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  return backend_->glyphOutline(font, glyphIndex, scale);
}

bool TextEngine::isBitmapOnly(FontHandle font) const {
  return backend_->isBitmapOnly(font);
}

std::optional<TextBackend::BitmapGlyph> TextEngine::bitmapGlyph(FontHandle font, int glyphIndex,
                                                                float scale) const {
  return backend_->bitmapGlyph(font, glyphIndex, scale);
}

std::optional<double> TextEngine::measureChUnitInEm(std::span<const RcString> fontFamilies) {
  FontHandle font;
  for (const auto& family : fontFamilies) {
    font = fontManager_.findFont(family);
    if (font) {
      break;
    }
  }
  if (!font) {
    font = fontManager_.fallbackFont();
  }

  TextBackendSimple measurementBackend(fontManager_, registry_);
  const auto shaped =
      measurementBackend.shapeRun(font, 1.0f, "0", 0, 1, false, FontVariant::Normal, false);
  if (shaped.glyphs.empty()) {
    return std::nullopt;
  }
  return shaped.glyphs.front().xAdvance;
}

const components::ComputedTextGeometryComponent& TextEngine::ensureComputedTextGeometryComponent(
    EntityHandle handle) const {
  const Entity textRootEntity = findTextRootEntity(handle);
  UTILS_RELEASE_ASSERT_MSG(textRootEntity != entt::null, "Text content element has no text root");

  // Return cached component if it already exists.
  if (const auto* existing =
          registry_.try_get<components::ComputedTextGeometryComponent>(textRootEntity)) {
    return *existing;
  }

  auto emptyAndReturn = [&]() -> const components::ComputedTextGeometryComponent& {
    return registry_.emplace_or_replace<components::ComputedTextGeometryComponent>(textRootEntity);
  };

  const auto* computedText = registry_.try_get<components::ComputedTextComponent>(textRootEntity);
  const auto* textComp = registry_.try_get<components::TextComponent>(textRootEntity);
  const auto* style = registry_.try_get<components::ComputedStyleComponent>(textRootEntity);
  if (!computedText || !textComp || !style || !style->properties) {
    return emptyAndReturn();
  }

  components::ComputedTextComponent styledText = *computedText;
  const EntityHandle rootHandle(registry_, textRootEntity);
  const TextLayoutParams params = buildTextLayoutParams(registry_, rootHandle, *style, *textComp);
  ResolvePerSpanLayoutStyles(registry_, styledText, params.viewBox, params.fontMetrics);

  const std::vector<TextRun> runs = const_cast<TextEngine*>(this)->layout(styledText, params);

  components::ComputedTextGeometryComponent cache;
  bool hasInkBounds = false;
  bool hasEmBoxBounds = false;

  const float fontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  for (size_t runIndex = 0; runIndex < runs.size() && runIndex < styledText.spans.size();
       ++runIndex) {
    const auto& run = runs[runIndex];
    const auto& span = styledText.spans[runIndex];
    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    if (spanText.empty()) {
      continue;
    }

    float runFontSizePx = fontSizePx;
    if (span.fontSize.value != 0.0) {
      runFontSizePx = static_cast<float>(
          span.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
    }

    const float runScale = run.font ? scaleForPixelHeight(run.font, runFontSizePx) : 0.0f;
    double emTop = static_cast<double>(runFontSizePx);
    double emBottom = 0.0;
    if (run.font && runScale > 0.0f) {
      const FontVMetrics metrics = fontVMetrics(run.font);
      emTop = static_cast<double>(metrics.ascent) * runScale;
      emBottom = -static_cast<double>(metrics.descent) * runScale;
    }

    bool firstChar = true;
    bool prevWasZwj = false;
    size_t pos = 0;
    std::vector<size_t> byteToApiCharIdx(spanText.size(), 0);
    size_t apiCharIdx = 0;
    while (pos < spanText.size()) {
      const size_t start = pos;
      const uint32_t cp = decodeUtf8(spanText, pos);
      const bool nonSpacing = isNonSpacing(cp) || prevWasZwj;
      prevWasZwj = (cp == 0x200D);
      if (!firstChar && !nonSpacing) {
        ++apiCharIdx;
      }
      for (size_t i = start; i < pos; ++i) {
        byteToApiCharIdx[i] = apiCharIdx;
      }
      firstChar = false;
    }

    const size_t localCharCount = firstChar ? 0 : apiCharIdx + 1;
    const size_t charBaseIndex = cache.characters.size();
    cache.characters.resize(charBaseIndex + localCharCount);
    for (size_t i = 0; i < localCharCount; ++i) {
      cache.characters[charBaseIndex + i].sourceEntity = span.sourceEntity;
    }

    if (!run.glyphs.empty()) {
      Boxd runEmBounds =
          Boxd::FromXYWH(run.glyphs.front().xPosition, run.glyphs.front().yPosition - emTop, 0.0,
                         emTop + emBottom);
      for (const auto& glyph : run.glyphs) {
        runEmBounds.addPoint(Vector2d(glyph.xPosition, glyph.yPosition - emTop));
        runEmBounds.addPoint(
            Vector2d(glyph.xPosition + glyph.xAdvance, glyph.yPosition + emBottom));
      }
      addBox(cache.emBoxBounds, hasEmBoxBounds, runEmBounds);
    }

    for (const auto& glyph : run.glyphs) {
      const size_t localCharIndex =
          glyph.cluster < byteToApiCharIdx.size() ? byteToApiCharIdx[glyph.cluster] : 0;
      if (localCharIndex >= localCharCount) {
        continue;
      }

      auto& charGeom = cache.characters[charBaseIndex + localCharIndex];
      if (!charGeom.rendered) {
        charGeom.startPosition = Vector2d(glyph.xPosition, glyph.yPosition);
        charGeom.rotation = glyph.rotateDegrees;
        charGeom.rendered = true;
      }

      charGeom.endPosition =
          Vector2d(glyph.xPosition + glyph.xAdvance, glyph.yPosition + glyph.yAdvance);
      charGeom.advance += std::hypot(glyph.xAdvance, glyph.yAdvance);

      const float emScale = run.font ? scaleForEmToPixels(run.font, runFontSizePx) : 0.0f;
      PathSpline glyphPath =
          glyphOutline(run.font, glyph.glyphIndex, emScale * glyph.fontSizeScale);
      if (!glyphPath.empty()) {
        Transformd glyphFromLocal = Transformd::Translate(glyph.xPosition, glyph.yPosition);
        if (glyph.rotateDegrees != 0.0) {
          glyphFromLocal =
              Transformd::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
              glyphFromLocal;
        }

        PathSpline transformed = transformPathSpline(glyphPath, glyphFromLocal);
        const Boxd extent = transformed.bounds();
        cache.glyphs.push_back({span.sourceEntity, std::move(transformed), extent});
        addBox(cache.inkBounds, hasInkBounds, extent);
        addBox(charGeom.extent, charGeom.hasExtent, extent);
      } else if (auto bitmap = bitmapGlyph(run.font, glyph.glyphIndex, emScale)) {
        const double targetX = glyph.xPosition + bitmap->bearingX;
        const double targetY = glyph.yPosition - bitmap->bearingY;
        const Boxd extent = Boxd::FromXYWH(targetX, targetY, bitmap->width * bitmap->scale,
                                           bitmap->height * bitmap->scale);
        addBox(cache.inkBounds, hasInkBounds, extent);
        addBox(charGeom.extent, charGeom.hasExtent, extent);
      }
    }
  }

  cache.runs = runs;

  return registry_.emplace_or_replace<components::ComputedTextGeometryComponent>(textRootEntity,
                                                                             std::move(cache));
}

std::vector<PathSpline> TextEngine::computedGlyphPaths(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  std::vector<PathSpline> result;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      result.push_back(glyph.path);
    }
  }
  return result;
}

Boxd TextEngine::computedInkBounds(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  Boxd result;
  bool initialized = false;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      addBox(result, initialized, glyph.extent);
    }
  }
  return result;
}

Boxd TextEngine::computedObjectBoundingBox(EntityHandle handle) const {
  const Entity rootEntity = findTextRootEntity(handle);
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  return handle.entity() == rootEntity ? cache.emBoxBounds : computedInkBounds(handle);
}

long TextEngine::getNumberOfChars(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  return static_cast<long>(filteredCharacters(registry_, handle, cache).size());
}

double TextEngine::getComputedTextLength(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  double total = 0.0;
  for (const auto* character : filteredCharacters(registry_, handle, cache)) {
    total += character->advance;
  }
  return total;
}

double TextEngine::getSubStringLength(EntityHandle handle, std::size_t charnum,
                                      std::size_t nchars) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  double total = 0.0;
  for (size_t i = charnum; i < characters.size() && i < charnum + nchars; ++i) {
    total += characters[i]->advance;
  }
  return total;
}

Vector2d TextEngine::getStartPositionOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Vector2d();
  }
  return characters[charnum]->startPosition;
}

Vector2d TextEngine::getEndPositionOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Vector2d();
  }
  return characters[charnum]->endPosition;
}

Boxd TextEngine::getExtentOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Boxd();
  }
  return characters[charnum]->extent;
}

double TextEngine::getRotationOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return 0.0;
  }
  return characters[charnum]->rotation;
}

long TextEngine::getCharNumAtPosition(EntityHandle handle, const Vector2d& point) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  for (size_t i = 0; i < characters.size(); ++i) {
    if (characters[i]->extent.contains(point)) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

}  // namespace donner::svg
