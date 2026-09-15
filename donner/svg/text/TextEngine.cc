#include "donner/svg/text/TextEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>

#include "donner/base/MathUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontMetricDependenciesComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
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

/// Converts a computed font size only when the result is representable by the shaping API.
float CheckedFontSizePx(double sizePx) {
  if (!std::isfinite(sizePx) || sizePx <= 0.0 ||
      sizePx > static_cast<double>(std::numeric_limits<float>::max())) {
    return 0.0f;
  }
  const float result = static_cast<float>(sizePx);
  return std::isfinite(result) ? result : 0.0f;
}

/// Zero or unrepresentable used sizes consume positioning without producing glyphs.
bool HasRenderableSpanText(std::string_view text, float usedSizePx) {
  return !text.empty() && usedSizePx > 0.0f && std::isfinite(usedSizePx);
}

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
 * Returns the face for the first family in @p families that a registered `@font-face` rule or the
 * font provider actually supplies.
 *
 * The availability test is separate from the lookup because \ref FontManager::findFont answers an
 * unknown family with the embedded fallback rather than an invalid handle; without it a list like
 * `Invalid, Noto Sans` would stop at its first entry and never reach the family that exists.
 *
 * @param fontManager Font manager to query.
 * @param families Font families in cascade order, highest priority first.
 * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
 * @param style CSS font-style value (0=normal, 1=italic, 2=oblique).
 * @param stretch CSS font-stretch value (1-9, 5=normal, matching FontStretch).
 * @return The first available face, or an invalid handle when no family is available.
 */
FontHandle FindFirstAvailableFont(FontManager& fontManager, std::span<const RcString> families,
                                  int weight = 400, int style = 0, int stretch = 5) {
  for (const RcString& family : families) {
    if (!fontManager.hasFamily(family)) {
      continue;
    }
    if (const FontHandle candidate = fontManager.findFont(family, weight, style, stretch)) {
      return candidate;
    }
  }

  return {};
}

/// Substitute the trusted embedded fallback before a backend can see unsupported font bytes.
FontHandle selectBackendSafeFont(const TextBackend& backend, FontManager& fontManager,
                                 FontHandle font) {
  if (font && backend.requiresTrustedFontData() && !fontManager.isTrustedFont(font)) {
    return fontManager.fallbackFont();
  }
  return font;
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

Path transformPath(const Path& spline, const Transform2d& transform) {
  PathBuilder builder;
  const auto& points = spline.points();

  for (const auto& command : spline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                        transform.transformPosition(points[command.pointIndex + 1]),
                        transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }

  return builder.build();
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

  params.fontFamilies = properties.fontFamily.get().value();
  params.fontSize = properties.fontSize.get().value();
  params.viewBox = components::LayoutSystem().getViewBox(handle);

  const FontMetrics baseFontMetrics = FontMetrics::DefaultsWithFontSize(12.0);
  const double fontSizePx =
      params.fontSize.toPixels(params.viewBox, baseFontMetrics, Lengthd::Extent::Mixed);
  params.fontMetrics = FontMetrics::DefaultsWithFontSize(fontSizePx);

  params.textAnchor = properties.textAnchor.get().value();
  params.writingMode = properties.writingMode.get().value();
  params.fontKerning = properties.fontKerning.get().value();
  params.fontSizeAdjust = properties.fontSizeAdjust.get().value();
  params.letterSpacingPx = properties.letterSpacing.get().value().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.wordSpacingPx = properties.wordSpacing.get().value().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.textLength = textComp.textLength;
  params.lengthAdjust = textComp.lengthAdjust;

  // SVG2 inline-size: resolve the length to pixels in the inline (X) axis. A value <= 0 means no
  // wrapping area. inline-size is non-inherited and applies to the <text> root, so it is read from
  // the root element's computed style here.
  const Lengthd inlineSize = properties.inlineSize.get().value();
  params.inlineSizePx = inlineSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  return params;
}

void ResolvePerSpanLayoutStyles(Registry& registry, components::ComputedTextComponent& text,
                                const Box2d& viewBox, const FontMetrics& fontMetrics) {
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

    span.textAnchor = style->properties->textAnchor.get().value();
    span.textDecoration = style->properties->textDecoration.get().value();
    span.baselineShift = style->properties->baselineShift.get().value();

    // Effective baseline alignment (matching resvg): a non-auto `alignment-baseline`
    // overrides; otherwise the span's (inherited) `dominant-baseline` applies.
    {
      DominantBaseline dominantBaseline = style->properties->dominantBaseline.get().value();
      if (dominantBaseline == DominantBaseline::NoChange) {
        // `no-change` uses the parent element's dominant baseline (one level, matching
        // resvg). A residual NoChange (parent is also no-change, or there is no parent
        // style) behaves like auto in computeBaselineShift.
        if (registry.all_of<donner::components::TreeComponent>(styleEntity)) {
          const Entity parent =
              registry.get<donner::components::TreeComponent>(styleEntity).parent();
          if (const auto* parentStyle =
                  parent != entt::null
                      ? registry.try_get<components::ComputedStyleComponent>(parent)
                      : nullptr;
              parentStyle && parentStyle->properties) {
            dominantBaseline = parentStyle->properties->dominantBaseline.get().value();
          }
        }
      }

      const DominantBaseline alignmentBaseline = style->properties->alignmentBaseline.get().value();
      span.alignmentBaseline =
          alignmentBaseline != DominantBaseline::Auto ? alignmentBaseline : dominantBaseline;
    }

    span.fontWeight = style->properties->fontWeight.get().value();
    span.fontStyle = style->properties->fontStyle.get().value();
    span.fontStretch = static_cast<FontStretch>(style->properties->fontStretch.get().value());
    span.fontVariant = style->properties->fontVariant.get().value();
    span.fontKerning = style->properties->fontKerning.get().value();
    span.fontSizeAdjust.emplace(style->properties->fontSizeAdjust.get().value());
    span.fontSize = style->properties->fontSize.get().value();
    span.fontFamilies = style->properties->fontFamily.get().value();
    span.visibility = style->properties->visibility.get().value();
    span.opacity = style->properties->opacity.get().value();
    span.letterSpacingPx = style->properties->letterSpacing.get().value().toPixels(
        viewBox, fontMetrics, Lengthd::Extent::X);
    span.wordSpacingPx = style->properties->wordSpacing.get().value().toPixels(viewBox, fontMetrics,
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

      // Resolve fresh each call: this runs per `draw()` and accumulates ancestor
      // shifts via push_back, so re-resolving the same span (e.g. two backends
      // drawing the same document, or any re-draw) would otherwise double the
      // nested baseline-shift. Clear so the layout is idempotent.
      span.ancestorBaselineShifts.clear();

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

        const Lengthd ancestorShift = ancestorStyle->properties->baselineShift.get().value();
        const double ancestorFontSizePx =
            ancestorStyle->properties->fontSize.get().value().toPixels(viewBox, fontMetrics,
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
  // Keyword → offset mapping matches resvg's usvg text layout
  // (ResolvedFont::alignment_baseline_shift), which in turn matches Chrome's hardcoded
  // baseline table. Positive values move glyphs down (+Y in SVG coordinates).
  switch (baseline) {
    case DominantBaseline::Auto:
    case DominantBaseline::Alphabetic:
    case DominantBaseline::UseScript:  // Deprecated SVG 1.1 keyword; behaves like auto.
    case DominantBaseline::NoChange:   // Resolved to the parent's value before layout; a
                                       // residual no-change behaves like auto.
    case DominantBaseline::ResetSize:  // Deprecated SVG 1.1 keyword; behaves like auto.
      return 0.0;
    case DominantBaseline::Middle: {
      // Half the x-height. Fonts without an OS/2 sxHeight fall back to 45% of
      // ascent−descent (what Firefox and resvg use).
      const double xHeight = vm.xHeight > 0 ? static_cast<double>(vm.xHeight)
                                            : static_cast<double>(vm.ascent - vm.descent) * 0.45;
      return xHeight * 0.5 * scale;
    }
    case DominantBaseline::Central:
      return static_cast<double>(vm.ascent + vm.descent) * 0.5 * scale;
    case DominantBaseline::Hanging: return static_cast<double>(vm.ascent) * 0.8 * scale;
    case DominantBaseline::Mathematical: return static_cast<double>(vm.ascent) * 0.5 * scale;
    case DominantBaseline::TextTop: return static_cast<double>(vm.ascent) * scale;
    case DominantBaseline::TextBottom:
    case DominantBaseline::Ideographic: return static_cast<double>(vm.descent) * scale;
  }
  return 0.0;
}

std::vector<ChunkRange> findChunkRanges(std::string_view spanText,
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

/// A typographic cluster keeps glyph-local shaping offsets separate from path placement.
struct TextPathCluster {
  size_t runIndex;
  size_t glyphStart;
  size_t glyphEnd;
  unsigned int charIndex;
  double pathOffset;
  double baselineOffset;
  double advance = 0.0;
};

/// Reusable path pen and cluster storage for the current textPath scope.
struct TextPathStagingState {
  double advance = 0.0;
  double dy = 0.0;
  std::vector<TextPathCluster> clusters;
};

/// Resolves a per-character displacement, returning zero when it is absent.
double TextPathDisplacement(const SmallVector<std::optional<Lengthd>, 1>& positions,
                            unsigned int charIndex, const TextLayoutParams& params,
                            Lengthd::Extent extent) {
  return charIndex < positions.size() && positions[charIndex]
             ? positions[charIndex]->toPixels(params.viewBox, params.fontMetrics, extent)
             : 0.0;
}

/// Returns whether this glyph ends its run-local typographic cluster.
bool EndsTextPathCluster(const TextRun& run, size_t glyphIndex, unsigned int charIndex,
                         const ByteIndexMappings& mappings) {
  if (glyphIndex + 1 == run.glyphs.size()) {
    return true;
  }
  const size_t nextByte = run.glyphs[glyphIndex + 1].cluster;
  return nextByte >= mappings.byteToCharIdx.size() || mappings.byteToCharIdx[nextByte] != charIndex;
}

/// Preserves glyph-local offsets while advancing one run's logical path clusters.
void StageTextPathRun(TextRun& run, size_t runIndex,
                      const components::ComputedTextComponent::TextSpan& span,
                      const ByteIndexMappings& mappings, const TextLayoutParams& params,
                      const TextBackend& backend, double defaultY, TextPathStagingState& state) {
  const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
  const bool vertical = isVertical(params.writingMode);
  for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
    TextGlyph& glyph = run.glyphs[gi];
    const unsigned int charIndex =
        glyph.cluster < mappings.byteToCharIdx.size() ? mappings.byteToCharIdx[glyph.cluster] : 0;
    if (gi > 0) {
      state.advance += glyph.xKern;
    }
    if (gi == 0 || charIndex != state.clusters.back().charIndex) {
      state.advance += TextPathDisplacement(span.dxList, charIndex, params, Lengthd::Extent::X);
      state.dy += TextPathDisplacement(span.dyList, charIndex, params, Lengthd::Extent::Y);
      state.clusters.push_back(
          {runIndex, gi, gi + 1, charIndex, state.advance, defaultY + state.dy});
    }
    TextPathCluster& cluster = state.clusters.back();
    glyph.xPosition = state.advance - cluster.pathOffset + (vertical ? 0.0 : glyph.xPosition);
    glyph.yPosition = vertical ? 0.0 : glyph.yPosition;
    state.advance += glyph.xAdvance;
    cluster.glyphEnd = gi + 1;
    cluster.advance = state.advance - cluster.pathOffset;
    if (EndsTextPathCluster(run, gi, charIndex, mappings)) {
      size_t byteIndex = run.glyphs[cluster.glyphStart].cluster;
      const uint32_t codepoint = decodeUtf8(spanText, byteIndex);
      if (!backend.isCursive(codepoint)) {
        state.advance += span.letterSpacingPx;
      }
      if (codepoint == 0x20) {
        state.advance += span.wordSpacingPx;
      }
    }
  }
}

/// A textLength owner contains typographic clusters and nested owners in document order.
struct PathLengthScope {
  struct Item {
    size_t index;
    bool scope;
    double start = 0.0;
    double end = 0.0;
    double position = 0.0;
  };
  std::optional<double> target;
  LengthAdjust adjust = LengthAdjust::Default;
  std::vector<Item> items;
  double start = 0.0;
  double end = 0.0;
  double length = 0.0;
  double scale = 1.0;
  double position = 0.0;
};

/// Adjusts path-local glyph positions before any path sampling, visiting each scope once.
void applyTextPathLengths(Registry& registry, const components::ComputedTextComponent& text,
                          const TextLayoutParams& params, size_t firstRun,
                          std::vector<TextRun>& runs, std::vector<TextPathCluster>& clusters) {
  std::vector<PathLengthScope> scopes(1);
  std::unordered_map<Entity, size_t> ownerScopes;
  const Entity pathEntity = text.spans[firstRun].textPathSourceEntity;
  auto setLength = [&](PathLengthScope& scope, const std::optional<Lengthd>& length,
                       LengthAdjust adjust) {
    if (length) {
      const double value = length->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
      if (std::isfinite(value) && value >= 0.0) {
        scope.target = value;
        scope.adjust = adjust;
      }
    }
  };
  if (pathEntity != entt::null && registry.valid(pathEntity)) {
    ownerScopes.emplace(pathEntity, 0);
    if (const auto* component = registry.try_get<components::TextComponent>(pathEntity)) {
      setLength(scopes[0], component->textLength, component->lengthAdjust);
    }
  }

  size_t clusterIndex = 0;
  for (size_t ri = firstRun; ri < runs.size(); ++ri) {
    const auto& span = text.spans[ri];
    size_t owner = 0;
    Entity current = span.sourceEntity;
    std::vector<Entity> ancestors;
    while (current != entt::null && registry.valid(current) && !ownerScopes.contains(current)) {
      ancestors.push_back(current);
      const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
      current = tree ? tree->parent() : entt::null;
    }
    if (const auto found = ownerScopes.find(current); found != ownerScopes.end()) {
      owner = found->second;
    }
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
      PathLengthScope scope;
      if (const auto* component = registry.try_get<components::TextComponent>(*it)) {
        setLength(scope, component->textLength, component->lengthAdjust);
      }
      if (scope.target) {
        const size_t child = scopes.size();
        scopes[owner].items.push_back({child, true});
        scopes.push_back(std::move(scope));
        owner = child;
      }
      ownerScopes.emplace(*it, owner);
    }
    // Direct layout callers may supply spans without a source tree.
    if (span.sourceEntity == entt::null && span.textLength) {
      PathLengthScope scope;
      setLength(scope, span.textLength, span.lengthAdjust);
      const size_t child = scopes.size();
      scopes[0].items.push_back({child, true});
      scopes.push_back(std::move(scope));
      owner = child;
    }
    while (clusterIndex < clusters.size() && clusters[clusterIndex].runIndex == ri) {
      const TextPathCluster& cluster = clusters[clusterIndex];
      scopes[owner].items.push_back(
          {clusterIndex, false, cluster.pathOffset, cluster.pathOffset + cluster.advance});
      ++clusterIndex;
    }
    runs[ri].textLengthAppliedOnPath = true;
  }

  // Children have larger indices, so both traversals are iterative and linear in the tree size.
  for (size_t si = scopes.size(); si-- > 0;) {
    PathLengthScope& scope = scopes[si];
    std::erase_if(scope.items, [&](const PathLengthScope::Item& item) {
      return item.scope && scopes[item.index].items.empty();
    });
    if (scope.items.empty()) {
      continue;
    }
    double fixedLength = 0.0;
    double cursor = 0.0;
    double previousEnd = 0.0;
    for (size_t ii = 0; ii < scope.items.size(); ++ii) {
      auto& item = scope.items[ii];
      if (item.scope) {
        item.start = scopes[item.index].start;
        item.end = scopes[item.index].end;
      }
      if (ii == 0) {
        scope.start = item.start;
      } else {
        cursor += item.start - previousEnd;
      }
      item.position = cursor;
      const double length = item.scope ? scopes[item.index].length : item.end - item.start;
      cursor += length;
      if (item.scope) {
        fixedLength += length;
      }
      previousEnd = item.end;
    }
    scope.end = previousEnd;
    scope.length = cursor;
    double extraSpacing = 0.0;
    const double adjustableLength = cursor - fixedLength;
    if (scope.target && cursor > 0.0) {
      if (scope.adjust == LengthAdjust::Spacing && scope.items.size() > 1) {
        extraSpacing = (*scope.target - cursor) / static_cast<double>(scope.items.size() - 1);
      } else if (scope.adjust == LengthAdjust::SpacingAndGlyphs && adjustableLength > 0.0) {
        const double scale = std::max(0.0, *scope.target - fixedLength) / adjustableLength;
        if (std::isfinite(scale) && scale <= std::numeric_limits<float>::max()) {
          scope.scale = scale;
        }
      }
    }
    cursor = 0.0;
    previousEnd = scope.start;
    for (auto& item : scope.items) {
      cursor += (item.start - previousEnd) * scope.scale;
      item.position = cursor;
      cursor += item.scope ? scopes[item.index].length : (item.end - item.start) * scope.scale;
      cursor += extraSpacing;
      previousEnd = item.end;
    }
    scope.length = cursor - extraSpacing;
  }
  scopes[0].position = scopes[0].start;
  for (PathLengthScope& scope : scopes) {
    for (const auto& item : scope.items) {
      const double position = scope.position + item.position;
      if (item.scope) {
        scopes[item.index].position = position;
      } else {
        TextPathCluster& cluster = clusters[item.index];
        cluster.pathOffset = position;
        cluster.advance *= scope.scale;
        auto& run = runs[cluster.runIndex];
        for (size_t gi = cluster.glyphStart; gi < cluster.glyphEnd; ++gi) {
          TextGlyph& glyph = run.glyphs[gi];
          glyph.xPosition *= scope.scale;
          glyph.xAdvance *= scope.scale;
          glyph.stretchScaleX *= NarrowToFloat(scope.scale);
        }
      }
    }
  }
}

/// Returns whether a cluster starts a new horizontal path chunk.
bool HasAbsoluteTextPathX(const components::ComputedTextComponent& text,
                          const TextPathCluster& cluster) {
  const auto& positions = text.spans[cluster.runIndex].xList;
  return cluster.charIndex < positions.size() && positions[cluster.charIndex].has_value();
}

/// Applies absolute path coordinates without discarding the same cluster's relative displacement.
void ResetTextPathCoordinates(const components::ComputedTextComponent& text,
                              const TextLayoutParams& params,
                              std::vector<TextPathCluster>& clusters) {
  double xShift = 0.0;
  for (TextPathCluster& cluster : clusters) {
    const auto& span = text.spans[cluster.runIndex];
    if (HasAbsoluteTextPathX(text, cluster)) {
      const double dx =
          TextPathDisplacement(span.dxList, cluster.charIndex, params, Lengthd::Extent::X);
      xShift = span.xList[cluster.charIndex]->toPixels(params.viewBox, params.fontMetrics,
                                                       Lengthd::Extent::X) +
               dx - cluster.pathOffset;
    }
    cluster.pathOffset += xShift;
  }
}

/// Anchors each horizontal path chunk against all of its typographic extents.
void AnchorTextPathClusters(const components::ComputedTextComponent& text,
                            std::vector<TextPathCluster>& clusters) {
  for (size_t first = 0; first < clusters.size();) {
    size_t end = first + 1;
    while (end < clusters.size() && !HasAbsoluteTextPathX(text, clusters[end])) {
      ++end;
    }
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (size_t ci = first; ci < end; ++ci) {
      const TextPathCluster& cluster = clusters[ci];
      minimum = std::min({minimum, cluster.pathOffset, cluster.pathOffset + cluster.advance});
      maximum = std::max({maximum, cluster.pathOffset, cluster.pathOffset + cluster.advance});
    }
    const TextAnchor anchor = text.spans[clusters[first].runIndex].textAnchor;
    const double alignment = anchor == TextAnchor::Middle ? (minimum + maximum) / 2.0
                             : anchor == TextAnchor::End  ? maximum
                                                          : minimum;
    const double shift = clusters[first].pathOffset - alignment;
    for (size_t ci = first; ci < end; ++ci) {
      clusters[ci].pathOffset += shift;
    }
    first = end;
  }
}

/// Samples each cluster once and places all of its glyphs with their shaped offsets.
std::optional<Vector2d> PlaceTextPathClusters(const Path::MeasuredPath& path, double startOffset,
                                              std::vector<TextRun>& runs,
                                              const std::vector<TextPathCluster>& clusters) {
  std::optional<Vector2d> lastPosition;
  for (const TextPathCluster& cluster : clusters) {
    const double halfAdvance = cluster.advance * 0.5;
    const double midpoint = startOffset + cluster.pathOffset + halfAdvance;
    const auto sample = path.pointAtArcLength(midpoint);
    auto& run = runs[cluster.runIndex];
    if (sample.valid) {
      const Vector2d origin(sample.point.x - halfAdvance * std::cos(sample.angle) -
                                std::sin(sample.angle) * cluster.baselineOffset,
                            sample.point.y - halfAdvance * std::sin(sample.angle) +
                                std::cos(sample.angle) * cluster.baselineOffset);
      for (size_t gi = cluster.glyphStart; gi < cluster.glyphEnd; ++gi) {
        TextGlyph& glyph = run.glyphs[gi];
        const double angle = sample.angle + glyph.rotateDegrees * MathConstants<double>::kDegToRad;
        const double localX = glyph.xPosition;
        const double localY = glyph.yPosition;
        glyph.xPosition = origin.x + localX * std::cos(angle) - localY * std::sin(angle);
        glyph.yPosition = origin.y + localX * std::sin(angle) + localY * std::cos(angle);
        glyph.rotateDegrees += sample.angle * MathConstants<double>::kRadToDeg;
      }
      lastPosition = Vector2d(sample.point.x + cluster.advance, sample.point.y);
    } else {
      for (size_t gi = cluster.glyphStart; gi < cluster.glyphEnd; ++gi) {
        run.glyphs[gi].glyphIndex = 0;
      }
    }
  }
  return lastPosition;
}

/// Places shaped clusters after length adjustments and absolute coordinate resets.
Vector2d placeTextPath(Registry& registry, const components::ComputedTextComponent& text,
                       const TextLayoutParams& params, size_t firstRun, std::vector<TextRun>& runs,
                       std::vector<TextPathCluster>& clusters) {
  applyTextPathLengths(registry, text, params, firstRun, runs, clusters);
  ResetTextPathCoordinates(text, params, clusters);
  AnchorTextPathClusters(text, clusters);
  const auto& firstSpan = text.spans[firstRun];
  const Path::MeasuredPath path = firstSpan.pathSpline->measure();
  const std::optional<Vector2d> lastPosition =
      PlaceTextPathClusters(path, firstSpan.pathStartOffset, runs, clusters);
  if (lastPosition) {
    return *lastPosition;
  }
  const auto end = path.pointAtArcLength(path.pathLength());
  return end.valid ? end.point : Vector2d::Zero();
}

void applyTextLength(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents, const TextLayoutParams& params,
                     bool vertical, double currentPenX, double currentPenY,
                     TextLengthTraversalStats* traversalStats) {
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
    double carriedAdvanceDelta = 0.0;
    bool carryActive = false;

    for (size_t i = 0; i < runs.size() && i < text.spans.size(); ++i) {
      if (traversalStats) {
        ++traversalStats->runVisits;
      }
      auto& run = runs[i];
      const auto& span = text.spans[i];

      if (run.onPath) {
        carryActive = false;
        carriedAdvanceDelta = 0.0;
        if (run.textLengthAppliedOnPath) {
          continue;
        }
      } else if (carryActive) {
        const auto& inlinePositions = vertical ? span.yList : span.xList;
        std::optional<size_t> resetCharIndex;
        for (size_t charIndex = 0; charIndex < inlinePositions.size(); ++charIndex) {
          if (traversalStats) {
            ++traversalStats->inlinePositionVisits;
          }
          if (inlinePositions[charIndex].has_value()) {
            resetCharIndex = charIndex;
            break;
          }
        }

        std::optional<ByteIndexMappings> mappings;
        if (resetCharIndex.has_value() && !run.glyphs.empty()) {
          const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
          if (traversalStats) {
            traversalStats->mappedTextBytes += spanText.size();
          }
          mappings = buildByteIndexMappings(spanText);
        }

        for (auto& glyph : run.glyphs) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          if (resetCharIndex.has_value()) {
            const size_t charIndex = glyph.cluster < mappings->byteToCharIdx.size()
                                         ? mappings->byteToCharIdx[glyph.cluster]
                                         : 0;
            if (charIndex >= *resetCharIndex) {
              continue;
            }
          }

          if (vertical) {
            glyph.yPosition += carriedAdvanceDelta;
          } else {
            glyph.xPosition += carriedAdvanceDelta;
          }
        }

        if (resetCharIndex.has_value()) {
          carryActive = false;
          carriedAdvanceDelta = 0.0;
        }
      }

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

      if (targetLength < 0.0) {
        continue;
      }

      if (span.lengthAdjust == LengthAdjust::Spacing) {
        const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
        const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
        for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          if (vertical) {
            run.glyphs[gi].yPosition += extraPerGap * static_cast<double>(gi);
          } else {
            run.glyphs[gi].xPosition += extraPerGap * static_cast<double>(gi);
          }
        }
      } else {
        const double scaleFactor = targetLength / naturalLength;
        for (auto& g : run.glyphs) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          if (vertical) {
            g.yPosition = runStartPos + (g.yPosition - runStartPos) * scaleFactor;
            g.yAdvance *= scaleFactor;
            g.stretchScaleY *= NarrowToFloat(scaleFactor);
          } else {
            g.xPosition = runStartPos + (g.xPosition - runStartPos) * scaleFactor;
            g.xAdvance *= scaleFactor;
            g.stretchScaleX *= NarrowToFloat(scaleFactor);
          }
        }
      }

      if (!run.onPath) {
        const double advanceDelta = targetLength - naturalLength;
        if (carryActive) {
          carriedAdvanceDelta += advanceDelta;
        } else {
          carriedAdvanceDelta = advanceDelta;
        }
        carryActive = true;
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

    if (targetLength >= 0.0) {
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
              g.stretchScaleY *= NarrowToFloat(scaleFactor);
            } else {
              g.xPosition = globalStartX + (g.xPosition - globalStartX) * scaleFactor;
              g.xAdvance *= scaleFactor;
              g.stretchScaleX *= NarrowToFloat(scaleFactor);
            }
          }
        }
      }
    }
  }
}

/// Fix up chunk text-anchors and apply per-chunk text-anchor adjustment.
void applyTextAnchor(std::vector<TextRun>& runs, std::vector<ChunkBoundary>& chunkBoundaries,
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
      if (runs[ri].onPath) {
        continue;  // On-path runs already have text-anchor applied along the path.
      }
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
      if (runs[ri].onPath) {
        continue;  // On-path runs already have text-anchor applied along the path.
      }
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
double computeSpanBaselineShiftPx(const TextBackend& backend,
                                  const components::ComputedTextComponent::TextSpan& span,
                                  FontHandle spanFont, float spanScale,
                                  const TextLayoutParams& params) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  FontMetrics spanFontMetrics = params.fontMetrics;
  const float spanFontSizePx =
      span.fontSize.value != 0.0 ? CheckedFontSizePx(span.fontSize.toPixels(
                                       params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed))
                                 : CheckedFontSizePx(params.fontSize.toPixels(
                                       params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
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
            backend.scaleForEmToPixels(spanFont, CheckedFontSizePx(ancestor.fontSizePx));
        spanBaselineShiftPx += -static_cast<double>(subSuper->subscriptYOffset) * ancestorScale;
      } else if (subSuper.has_value() && ancestor.keyword == BSK::Super) {
        const float ancestorScale =
            backend.scaleForEmToPixels(spanFont, CheckedFontSizePx(ancestor.fontSizePx));
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

bool applyInlineSizeWrap(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                         const TextLayoutParams& params, double measurePx, double lineHeightPx) {
  if (measurePx <= 0.0) {
    return false;
  }

  // A flat, document-order view of every rendered glyph, tagged with whether it is a soft-wrap
  // opportunity (whitespace). Runs with no glyphs (hidden/empty spans) contribute nothing.
  struct FlatGlyph {
    size_t run;
    size_t glyph;
    double origX;    ///< Original xPosition from the flat single-line layout.
    double advance;  ///< Glyph xAdvance.
    double origY;    ///< Original yPosition (carries per-glyph baseline-shift).
    bool isSpace;    ///< Whitespace: a soft-wrap opportunity between words.
  };

  std::vector<FlatGlyph> flat;
  for (size_t ri = 0; ri < runs.size() && ri < text.spans.size(); ++ri) {
    const auto& run = runs[ri];
    if (run.onPath) {
      return false;  // inline-size does not combine with text-on-path.
    }
    const auto& span = text.spans[ri];
    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
    for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
      const auto& g = run.glyphs[gi];
      size_t clusterPos = g.cluster;
      const uint32_t cp = clusterPos < spanText.size() ? decodeUtf8(spanText, clusterPos) : 0;
      const bool isSpace = (cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D);
      flat.push_back({ri, gi, g.xPosition, g.xAdvance, g.yPosition, isSpace});
    }
  }

  if (flat.size() < 2) {
    return false;  // Nothing to wrap.
  }

  const double originX = flat.front().origX;
  const double originY = flat.front().origY;

  // ── Phase A: assign each glyph a line index via greedy word wrapping ──
  std::vector<size_t> lineOf(flat.size(), 0);
  size_t line = 0;
  double lineWidthUsed = 0.0;  // Committed width on the current line, excluding trailing spaces.
  double pendingSpace = 0.0;   // Width of spaces since the last committed word.

  size_t i = 0;
  while (i < flat.size()) {
    if (flat[i].isSpace) {
      // Spaces stay (tentatively) on the current line; their width is pending until the next
      // word commits it or a wrap discards it (trailing spaces hang).
      lineOf[i] = line;
      pendingSpace += flat[i].advance;
      ++i;
      continue;
    }

    // Gather a maximal word (run of non-space glyphs).
    const size_t wordStart = i;
    while (i < flat.size() && !flat[i].isSpace) {
      ++i;
    }
    const size_t wordEnd = i;  // Exclusive.
    const double wordWidth =
        (flat[wordEnd - 1].origX + flat[wordEnd - 1].advance) - flat[wordStart].origX;

    if (lineWidthUsed > 0.0 && lineWidthUsed + pendingSpace + wordWidth > measurePx + 1e-3) {
      // Break before this word: start a new line. Pending trailing spaces hang on the old line.
      ++line;
      lineWidthUsed = wordWidth;
    } else if (lineWidthUsed == 0.0) {
      lineWidthUsed = wordWidth;  // First word on the line: no leading space counted.
    } else {
      lineWidthUsed += pendingSpace + wordWidth;
    }
    pendingSpace = 0.0;

    for (size_t w = wordStart; w < wordEnd; ++w) {
      lineOf[w] = line;
    }
  }

  const size_t numLines = line + 1;
  if (numLines < 2) {
    return false;  // Everything fit on one line: leave the flat layout (and its global
                   // text-anchor pass) untouched.
  }

  // ── Phase B: position glyphs line by line, preserving intra-line spacing ──
  std::vector<double> newX(flat.size(), 0.0);
  for (size_t k = 0; k < flat.size(); ++k) {
    if (k == 0 || lineOf[k] != lineOf[k - 1]) {
      newX[k] = originX;  // First glyph of a line starts at the block origin.
    } else {
      newX[k] = newX[k - 1] + (flat[k].origX - flat[k - 1].origX);
    }
  }

  // ── Phase C: per-line text-anchor shift ──
  // Each line's used width is the right edge of its last non-space glyph minus the origin.
  std::vector<double> lineRightEdge(numLines, originX);
  for (size_t k = 0; k < flat.size(); ++k) {
    if (!flat[k].isSpace) {
      lineRightEdge[lineOf[k]] = std::max(lineRightEdge[lineOf[k]], newX[k] + flat[k].advance);
    }
  }

  std::vector<double> lineShift(numLines, 0.0);
  for (size_t l = 0; l < numLines; ++l) {
    const double used = lineRightEdge[l] - originX;
    if (params.textAnchor == TextAnchor::Middle) {
      lineShift[l] = -used / 2.0;
    } else if (params.textAnchor == TextAnchor::End) {
      lineShift[l] = -used;
    }
  }

  // ── Write back positions ──
  for (size_t k = 0; k < flat.size(); ++k) {
    const size_t l = lineOf[k];
    auto& g = runs[flat[k].run].glyphs[flat[k].glyph];
    g.xPosition = newX[k] + lineShift[l];
    // Preserve each glyph's baseline-shift delta from the original baseline, then stack the line.
    g.yPosition = originY + static_cast<double>(l) * lineHeightPx + (flat[k].origY - originY);
  }

  return true;
}

}  // namespace text_engine_detail

using namespace text_engine_detail;  // NOLINT(google-build-using-namespace)

namespace {

void addBox(Box2d& accum, bool& initialized, const Box2d& box) {
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
    Registry& registry, EntityHandle handle,
    const components::ComputedTextGeometryComponent& cache) {
  std::vector<const components::ComputedTextGeometryComponent::CharacterGeometry*> result;
  for (const auto& character : cache.characters) {
    if (character.rendered && isDescendantOf(registry, character.sourceEntity, handle.entity())) {
      result.push_back(&character);
    }
  }
  return result;
}

template <typename Geometry>
bool FontDependenciesChanged(Geometry& geometry, uint64_t revision,
                             std::span<const FontFaceDependency> resolved) {
  if (geometry.fontResourceRevision == revision) return false;
  geometry.fontResourceRevision = revision;
  for (const auto& dependency : geometry.fontDependencies) {
    const auto current = std::find_if(resolved.begin(), resolved.end(), [&](const auto& face) {
      return face.family == dependency.family && face.request == dependency.request;
    });
    if (current == resolved.end() ||
        (current->state == FontFaceLoadState::Loaded &&
         (dependency.state != FontFaceLoadState::Loaded ||
          dependency.availability.contentId != current->availability.contentId ||
          dependency.availability.contentGeneration != current->availability.contentGeneration))) {
      return true;
    }
  }
  return false;
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

bool TextEngine::needsFontResourceRefresh() const {
  return fontManager_.fontResourceRevision() != observedFontResourceRevision_ ||
         fontManager_.needsResourceRefresh();
}

std::vector<Entity> TextEngine::refreshFontResources() {
  fontManager_.refreshPendingFonts();
  const uint64_t revision = fontManager_.fontResourceRevision();
  const auto resolved = fontManager_.faceDependencies();

  std::vector<Entity> changedRoots;
  for (auto view = registry_.view<components::ComputedTextGeometryComponent>();
       const Entity root : view) {
    if (FontDependenciesChanged(view.get<components::ComputedTextGeometryComponent>(root), revision,
                                resolved))
      changedRoots.push_back(root);
  }
  for (const Entity root : changedRoots) {
    components::InvalidateTextLayout(EntityHandle(registry_, root));
  }
  for (auto view = registry_.view<components::FontMetricDependenciesComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::FontMetricDependenciesComponent>(entity),
                                 revision, resolved))
      continue;
    registry_.remove<components::ComputedPathComponent>(entity);
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::Shape | components::DirtyFlagsComponent::LayoutCascade |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  for (auto view = registry_.view<components::ComputedClipPathsComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::ComputedClipPathsComponent>(entity), revision,
                                 resolved))
      continue;
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::TextGeometry | components::DirtyFlagsComponent::Paint |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  for (auto view = registry_.view<components::FontPaintDependenciesComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::FontPaintDependenciesComponent>(entity),
                                 revision, resolved))
      continue;
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::TextGeometry | components::DirtyFlagsComponent::Filter |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  observedFontResourceRevision_ = revision;
  return changedRoots;
}

void TextEngine::prepareForElement(EntityHandle handle, ParseWarningSink& outWarnings) {
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
      resourceManager->synchronizeStylesheetFontFaces(
          entity, view.get<components::StylesheetComponent>(entity).stylesheet.fontFaces());
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

namespace {

/**
 * @brief The glyphs of \p run whose painted geometry belongs in the text geometry cache.
 *
 * `visibility: hidden` and `visibility: collapse` suppress painting only. The span is still laid
 * out and keeps its positioned glyphs so it contributes to the element's object bounding box, per
 * the SVG object-bounding-box definition, but it produces no ink geometry and no per-character
 * paint records. Returns an empty span for such a run.
 *
 * @param span The span that produced \p run.
 * @param run The positioned layout run.
 * @return The glyphs to record as painted, which is empty when the span is not painted.
 */
std::span<const TextGlyph> PaintedSpanGlyphs(
    const components::ComputedTextComponent::TextSpan& span, const TextRun& run) {
  return span.visibility == Visibility::Visible ? std::span<const TextGlyph>(run.glyphs)
                                                : std::span<const TextGlyph>();
}

/// Resolves a span's inherited font family and face attributes.
FontHandle ResolveSpanFace(FontManager& fontManager,
                           const components::ComputedTextComponent::TextSpan& span,
                           const SmallVector<RcString, 1>& families, FontHandle fallback) {
  const SmallVector<RcString, 1>& spanFamilies =
      span.fontFamilies.empty() ? families : span.fontFamilies;
  FontHandle spanFont = FindFirstAvailableFont(fontManager, spanFamilies);
  if (!spanFont) {
    spanFont = fallback;
  }
  if (span.fontWeight != 400 || span.fontStyle != FontStyle::Normal ||
      span.fontStretch != FontStretch::Normal) {
    const FontHandle candidate = FindFirstAvailableFont(fontManager, spanFamilies, span.fontWeight,
                                                        static_cast<int>(span.fontStyle),
                                                        static_cast<int>(span.fontStretch));
    if (candidate) {
      spanFont = candidate;
    }
  }

  return spanFont;
}

/// Applies the requested x-height ratio after the final face has been selected.
float AdjustFontSize(const TextBackend& backend, FontHandle font, float sizePx,
                     const std::optional<double>& fontSizeAdjust) {
  sizePx = CheckedFontSizePx(sizePx);
  if (sizePx == 0.0f || !fontSizeAdjust) return sizePx;
  if (!std::isfinite(*fontSizeAdjust) || *fontSizeAdjust <= 0.0) return 0.0f;
  const FontVMetrics metrics = backend.fontVMetrics(font);
  if (metrics.xHeight <= 0 || metrics.unitsPerEm <= 0) return sizePx;
  const double aspect = static_cast<double>(metrics.xHeight) / metrics.unitsPerEm;
  return CheckedFontSizePx(static_cast<double>(sizePx) * *fontSizeAdjust / aspect);
}

/// Preserve same-family boundary shaping across size/face changes, using the preceding face's kern.
bool CompatibleKerningRuns(const FontManager& fontManager, const TextRun& current,
                           FontVariant currentVariant, FontHandle previousFont,
                           float previousSizePx, FontVariant previousVariant, bool currentKerning,
                           bool previousKerning) {
  return currentKerning && previousKerning && currentVariant == previousVariant &&
         current.usedFontSizePx > 0.0f && previousSizePx > 0.0f &&
         fontManager.fontsShareFamily(current.font, previousFont);
}

/// Keep source clusters in the positioning pipeline without invoking a font backend at size zero.
TextBackend::ShapedRun ShapeAddressableZeroSizeText(std::string_view text, size_t start,
                                                    size_t length) {
  TextBackend::ShapedRun result;
  const size_t end = start + length;
  for (size_t offset = start; offset < end;) {
    TextBackend::ShapedGlyph glyph;
    glyph.cluster = static_cast<uint32_t>(offset);
    result.glyphs.push_back(glyph);
    decodeUtf8(text, offset);
  }
  return result;
}

/// Select zero-advance character records or the normal kerning-aware shaping path.
TextBackend::ShapedRun ShapeSpanChunk(const TextBackend& backend, const TextRun& run,
                                      std::string_view text, const ChunkRange& chunk, bool vertical,
                                      FontVariant variant, bool kerning) {
  if (run.usedFontSizePx == 0.0f) {
    return ShapeAddressableZeroSizeText(text, chunk.byteStart, chunk.byteEnd - chunk.byteStart);
  }
  return kerning
             ? backend.shapeRun(run.font, run.usedFontSizePx, text, chunk.byteStart,
                                chunk.byteEnd - chunk.byteStart, vertical, variant, false)
             : backend.shapeRunNoKerning(run.font, run.usedFontSizePx, text, chunk.byteStart,
                                         chunk.byteEnd - chunk.byteStart, vertical, variant, false);
}

/// Valid zero adjustment retains characters; other unusable sizes do not reach layout.
bool HasAddressableSpanText(std::string_view text, float size,
                            const std::optional<double>& adjustment) {
  return !text.empty() && (HasRenderableSpanText(text, size) || adjustment == 0.0);
}

/// Nonempty unshaped text interrupts kerning; an empty span keeps the previous pair.
void UpdateUnshapedSpanPredecessor(std::string_view text, const TextRun& run, bool kerningEnabled,
                                   uint32_t& previousCodepoint, FontHandle& previousFont,
                                   float& previousSizePx, bool& previousKerning) {
  if (text.empty()) {
    return;
  }
  previousCodepoint = 0;
  previousFont = run.font;
  previousSizePx = run.usedFontSizePx;
  previousKerning = kerningEnabled;
}

}  // namespace

ResolvedTextFont TextEngine::resolveUsedFont(EntityHandle styleOwner, const Box2d& viewBox,
                                             const FontMetrics& fontMetrics) const {
  UTILS_RELEASE_ASSERT(styleOwner.registry() == &registry_);
  const auto* style = registry_.try_get<components::ComputedStyleComponent>(styleOwner.entity());
  if (!style || !style->properties) return {};
  const auto& properties = *style->properties;
  components::ComputedTextComponent::TextSpan span;
  span.fontFamilies = properties.fontFamily.get().value();
  span.fontWeight = properties.fontWeight.get().value();
  span.fontStyle = properties.fontStyle.get().value();
  span.fontStretch = static_cast<FontStretch>(properties.fontStretch.get().value());
  FontHandle font =
      ResolveSpanFace(fontManager_, span, span.fontFamilies, fontManager_.fallbackFont());
  font = selectBackendSafeFont(*backend_, fontManager_, font);
  const float size = CheckedFontSizePx(
      properties.fontSize.get().value().toPixels(viewBox, fontMetrics, Lengthd::Extent::Mixed));
  return {font, AdjustFontSize(*backend_, font, size, properties.fontSizeAdjust.get().value())};
}

std::vector<TextRun> TextEngine::layout(const components::ComputedTextComponent& text,
                                        const TextLayoutParams& params) {
  // ── Resolve base font ─────────────────────────────────────────────────────────
  FontHandle font = FindFirstAvailableFont(fontManager_, params.fontFamilies);
  if (!font) {
    font = fontManager_.fallbackFont();
  }

  const float fontSizePx = CheckedFontSizePx(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // ── Layout state ──────────────────────────────────────────────────────────────
  std::vector<TextRun> runs;
  bool haveCurrentPosition = false;
  double currentPenX = 0.0;
  double currentPenY = 0.0;
  double prevDefaultY = 0.0;  // Previous span's baseline-shift offset, for undo/redo between spans.
  uint32_t prevSpanLastCodepoint = 0;  // Last codepoint of previous span, for cross-span kerning.
  FontHandle prevSpanFont;
  float prevSpanFontSizePx = 0.0f;
  bool prevSpanFontKerning = true;
  FontVariant prevSpanFontVariant = FontVariant::Normal;
  Entity prevTextPathSource = entt::null;
  std::optional<size_t> firstPathRun;
  TextPathStagingState pathStaging;

  std::vector<ChunkBoundary> chunkBoundaries;
  std::vector<RunPenExtent> runExtents;

  auto finishTextPath = [&]() {
    if (firstPathRun) {
      const Vector2d end =
          placeTextPath(registry_, text, params, *firstPathRun, runs, pathStaging.clusters);
      pathStaging.clusters.clear();
      currentPenX = end.x;
      currentPenY = end.y;
      prevDefaultY = 0.0;
      haveCurrentPosition = true;
      firstPathRun.reset();
      prevTextPathSource = entt::null;
      pathStaging.advance = 0.0;
      pathStaging.dy = 0.0;
    }
  };

  // ── Per-span layout loop ──────────────────────────────────────────────────────
  for (const auto& span : text.spans) {
    if (firstPathRun && !span.hidden &&
        (!span.pathSpline || span.textPathSourceEntity == entt::null ||
         span.textPathSourceEntity != prevTextPathSource)) {
      finishTextPath();
    }
    TextRun run;

    // Hidden spans (display:none) are not rendered. Push empty run.
    // Also hide textPath spans whose href could not be resolved (SVG spec §10.12.1).
    if (span.hidden || span.textPathFailed) {
      runExtents.push_back({0.0, 0.0, 0.0, 0.0});
      runs.push_back(std::move(run));
      continue;
    }

    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    // ── Per-span font resolution ────────────────────────────────────────────────
    FontHandle spanFont = ResolveSpanFace(fontManager_, span, params.fontFamilies, font);

    // Per-span font size: use the span's fontSize if set, otherwise the text element's.
    float spanFontSizePx = span.fontSize.value != 0.0
                               ? CheckedFontSizePx(span.fontSize.toPixels(
                                     params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed))
                               : fontSizePx;

    const uint32_t spanTestCodepoint = firstNonAsciiCodepoint(spanText);
    spanFont = selectBackendSafeFont(*backend_, fontManager_, spanFont);
    spanFont = findCoverageFallbackFont(*backend_, fontManager_, spanFont, spanFontSizePx,
                                        spanTestCodepoint);
    const std::optional<double> fontSizeAdjust =
        span.fontSizeAdjust.value_or(params.fontSizeAdjust);
    spanFontSizePx = AdjustFontSize(*backend_, spanFont, spanFontSizePx, fontSizeAdjust);
    run.font = spanFont;
    run.usedFontSizePx = spanFontSizePx;

    // Note: bitmap-only fonts (e.g., color emoji) are valid for the full backend.
    // The simple backend can't handle them but produces empty shapeRun results,
    // which the renderer gracefully skips.

    const float spanScale = backend_->scaleForEmToPixels(spanFont, spanFontSizePx);

    // ── Per-span baseline-shift ─────────────────────────────────────────────────
    const double spanBaselineShiftPx =
        computeSpanBaselineShiftPx(*backend_, span, spanFont, spanScale, params);

    // ── Baseline alignment (dominant-baseline / alignment-baseline) ────────────
    // span.alignmentBaseline is the effective baseline resolved per span: a non-auto
    // alignment-baseline override, otherwise the span's (inherited) dominant-baseline.
    // Matching resvg, baseline alignment applies only to horizontal text.
    double effectiveBaselineShift = 0.0;
    if (!isVertical(params.writingMode) && span.alignmentBaseline != DominantBaseline::Auto) {
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
    if (span.pathSpline && !vertical) {
      yListLocal.clear();
    }
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

    const bool spanFontKerning = span.fontKerning.value_or(params.fontKerning) != FontKerning::None;
    // Propagate the span-start position even when the span produces no glyphs.
    if (!HasAddressableSpanText(spanText, spanFontSizePx, fontSizeAdjust)) {
      currentPenX = penX;
      currentPenY = penY;
      prevDefaultY = defaultY;
      haveCurrentPosition = true;
      UpdateUnshapedSpanPredecessor(spanText, run, spanFontKerning, prevSpanLastCodepoint,
                                    prevSpanFont, prevSpanFontSizePx, prevSpanFontKerning);
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
    bool prevChunkFontKerning = prevSpanFontKerning;

    for (size_t ci = 0; ci < chunkRanges.size(); ++ci) {
      const auto& chunk = chunkRanges[ci];
      const auto shaped = ShapeSpanChunk(*backend_, run, spanText, chunk, vertical,
                                         span.fontVariant, spanFontKerning);

      // ── RTL Y-override for multi-glyph chunks ─────────────────────────────────
      // When a multi-glyph RTL chunk starts because of an absolute y position on the
      // first DOM character, all glyphs in the chunk should share that y. HarfBuzz
      // returns RTL glyphs in visual order (last DOM char first), so the engine would
      // otherwise apply the explicit y to the wrong glyph. Pre-compute the override.
      std::optional<double> chunkYOverride;
      const bool isRTLChunk =
          shaped.glyphs.size() > 1 && shaped.glyphs.front().cluster > shaped.glyphs.back().cluster;
      if (isRTLChunk && ci > 0) {
        // The chunk started at chunk.byteStart - check if that byte's charIdx has absolute y.
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
            spanFontKerning && prevChunkFontKerning
                ? backend_->crossSpanKern(prevChunkFont, prevChunkFontSizePx, spanFont,
                                          spanFontSizePx, prevChunkLastCodepoint, firstCp, vertical)
                : 0.0;
        appliedCrossKern = true;
      } else if (ci == 0 && !span.startsNewChunk && prevSpanLastCodepoint != 0 &&
                 !shaped.glyphs.empty()) {
        // Cross-span kerning for the first chunk.
        size_t firstByteIdx = chunk.byteStart;
        const uint32_t firstCp = decodeUtf8(spanText, firstByteIdx);
        crossKern =
            CompatibleKerningRuns(fontManager_, run, span.fontVariant, prevSpanFont,
                                  prevSpanFontSizePx, prevSpanFontVariant, spanFontKerning,
                                  prevSpanFontKerning)
                ? backend_->crossSpanKern(prevSpanFont, prevSpanFontSizePx, spanFont,
                                          spanFontSizePx, prevSpanLastCodepoint, firstCp, vertical)
                : 0.0;
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
          // Path glyphs retain local shaping offsets until their cluster is placed.
          glyph.xPosition = span.pathSpline ? sg.xOffset : penX + sg.xOffset;
          glyph.yPosition = span.pathSpline ? sg.yOffset : penY + sg.yOffset;
          glyph.xAdvance = sg.xAdvance;
          glyph.yAdvance = sg.yAdvance;
          glyph.xKern = sg.xKern;
          glyph.fontSizeScale = sg.fontSizeScale;
          glyph.cluster = sg.cluster;

          // Per-character rotation (last value repeats per SVG spec).
          if (rawCpIdx < span.rotateList.size()) {
            glyph.rotateDegrees = span.rotateList[rawCpIdx];
          } else if (!span.rotateList.empty()) {
            glyph.rotateDegrees = span.rotateList.back();
          }

          // Rotate combining mark offsets around the base glyph so the cluster rotates together.
          if (!span.pathSpline && glyph.rotateDegrees != 0.0 && !run.glyphs.empty() &&
              glyph.xAdvance == 0.0) {
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
        prevChunkFontKerning = spanFontKerning;
      }
    }

    // Stage glyph clusters before placement of the complete textPath.
    if (span.pathSpline) {
      if (!firstPathRun) {
        firstPathRun = runs.size();
        prevTextPathSource = span.textPathSourceEntity;
      }
      StageTextPathRun(run, runs.size(), span, indexMappings, params, *backend_, defaultY,
                       pathStaging);
      run.onPath = true;
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
    prevSpanFontKerning = spanFontKerning;
    prevSpanFontVariant = span.fontVariant;

    runExtents.push_back({runPenStartX, runPenStartY, penX, penY});
    runs.push_back(std::move(run));
  }

  finishTextPath();

  // ── inline-size auto-flow (SVG2) ──────────────────────────────────────────────
  // Wrapping is supported only for horizontal writing modes (documented limitation). When it
  // applies it rewrites glyph positions into stacked lines and performs per-line text-anchor,
  // so the global text-anchor / textLength passes are skipped.
  bool wrapped = false;
  if (!runs.empty() && params.inlineSizePx > 0.0 && !isVertical(params.writingMode)) {
    // "normal" line-height derived from the base font's vertical metrics.
    const FontVMetrics vm = backend_->fontVMetrics(font);
    const float phScale = backend_->scaleForPixelHeight(font, fontSizePx);
    double lineHeightPx =
        static_cast<double>(vm.ascent - vm.descent + vm.lineGap) * static_cast<double>(phScale);
    if (!(lineHeightPx > 0.0)) {
      lineHeightPx = static_cast<double>(fontSizePx) * 1.2;
    }
    wrapped = applyInlineSizeWrap(runs, text, params, params.inlineSizePx, lineHeightPx);
  }

  // ── textLength and text-anchor adjustments ────────────────────────────────────
  if (!runs.empty() && !wrapped) {
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

std::optional<UnderlineMetrics> TextEngine::strikeoutMetrics(FontHandle font) const {
  return backend_->strikeoutMetrics(font);
}

std::optional<SubSuperMetrics> TextEngine::subSuperMetrics(FontHandle font) const {
  return backend_->subSuperMetrics(font);
}

Path TextEngine::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  return backend_->glyphOutline(font, glyphIndex, scale);
}

bool TextEngine::isBitmapOnly(FontHandle font) const {
  return backend_->isBitmapOnly(font);
}

std::optional<TextBackend::BitmapGlyph> TextEngine::bitmapGlyph(FontHandle font, int glyphIndex,
                                                                float scale) const {
  return backend_->bitmapGlyph(font, glyphIndex, scale);
}

std::optional<double> TextEngine::measureChUnitInEm(std::span<const RcString> fontFamilies,
                                                    Entity geometryOwner) {
  std::vector<FontFaceDependency> dependencies;
  const auto measured = [&]() -> std::optional<double> {
    const auto capture = fontManager_.captureDependencies(dependencies);
    FontHandle font = FindFirstAvailableFont(fontManager_, fontFamilies);
    if (!font) {
      font = fontManager_.fallbackFont();
    }

    TextBackendSimple measurementBackend(fontManager_, registry_);
    font = selectBackendSafeFont(measurementBackend, fontManager_, font);
    const auto shaped =
        measurementBackend.shapeRun(font, 1.0f, "0", 0, 1, false, FontVariant::Normal, false);
    if (shaped.glyphs.empty()) {
      return std::nullopt;
    }
    return shaped.glyphs.front().xAdvance;
  }();
  if (geometryOwner != entt::null) {
    if (dependencies.empty()) {
      registry_.remove<components::FontMetricDependenciesComponent>(geometryOwner);
    } else {
      registry_.emplace_or_replace<components::FontMetricDependenciesComponent>(
          geometryOwner, std::move(dependencies), fontManager_.fontResourceRevision());
    }
  }
  return measured;
}

const components::ComputedTextGeometryComponent& TextEngine::ensureComputedTextGeometryComponent(
    EntityHandle handle) const {
  const Entity textRootEntity = findTextRootEntity(handle);
  UTILS_RELEASE_ASSERT_MSG(textRootEntity != entt::null, "Text content element has no text root");

  // Return cached component if it already exists.
  if (const auto* existing =
          registry_.try_get<components::ComputedTextGeometryComponent>(textRootEntity)) {
    fontManager_.recordDependencies(existing->fontDependencies);
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

  components::ComputedTextGeometryComponent cache;
  std::vector<TextRun> runs;
  {
    const auto capture = fontManager_.captureDependencies(cache.fontDependencies);
    runs = const_cast<TextEngine*>(this)->layout(styledText, params);
  }
  cache.fontResourceRevision = fontManager_.fontResourceRevision();
  bool hasInkBounds = false;
  bool hasEmBoxBounds = false;

  for (size_t runIndex = 0; runIndex < runs.size() && runIndex < styledText.spans.size();
       ++runIndex) {
    const auto& run = runs[runIndex];
    const auto& span = styledText.spans[runIndex];
    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);

    if (spanText.empty()) {
      continue;
    }

    const float runFontSizePx = run.usedFontSizePx;

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
      Box2d runEmBounds =
          Box2d::FromXYWH(run.glyphs.front().xPosition, run.glyphs.front().yPosition - emTop, 0.0,
                          emTop + emBottom);
      for (const auto& glyph : run.glyphs) {
        runEmBounds.addPoint(Vector2d(glyph.xPosition, glyph.yPosition - emTop));
        runEmBounds.addPoint(
            Vector2d(glyph.xPosition + glyph.xAdvance, glyph.yPosition + emBottom));
      }
      addBox(cache.emBoxBounds, hasEmBoxBounds, runEmBounds);
    }

    for (const auto& glyph : PaintedSpanGlyphs(span, run)) {
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

      if (runFontSizePx == 0.0f) continue;

      const float emScale = run.font ? scaleForEmToPixels(run.font, runFontSizePx) : 0.0f;
      Path glyphPath = glyphOutline(run.font, glyph.glyphIndex, emScale * glyph.fontSizeScale);
      if (!glyphPath.empty()) {
        if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
          glyphPath = transformPath(glyphPath,
                                    Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
        }

        Transform2d glyphFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
        if (glyph.rotateDegrees != 0.0) {
          glyphFromLocal =
              Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
              glyphFromLocal;
        }

        Path transformed = transformPath(glyphPath, glyphFromLocal);
        const Box2d extent = transformed.bounds();
        cache.glyphs.push_back({span.sourceEntity, std::move(transformed), extent});
        addBox(cache.inkBounds, hasInkBounds, extent);
        addBox(charGeom.extent, charGeom.hasExtent, extent);
      } else if (auto bitmap = bitmapGlyph(run.font, glyph.glyphIndex, emScale)) {
        const double targetX = glyph.xPosition + bitmap->bearingX;
        const double targetY = glyph.yPosition - bitmap->bearingY;
        const Box2d extent = Box2d::FromXYWH(targetX, targetY, bitmap->width * bitmap->scale,
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

std::vector<Path> TextEngine::computedGlyphPaths(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  std::vector<Path> result;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      result.push_back(glyph.path);
    }
  }
  return result;
}

std::vector<TextEngine::GlyphOutline> TextEngine::computedGlyphOutlines(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  std::vector<GlyphOutline> result;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      result.push_back(GlyphOutline{glyph.path, glyph.sourceEntity});
    }
  }
  return result;
}

Box2d TextEngine::computedInkBounds(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  Box2d result;
  bool initialized = false;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      addBox(result, initialized, glyph.extent);
    }
  }
  return result;
}

Box2d TextEngine::computedObjectBoundingBox(EntityHandle handle) const {
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

Box2d TextEngine::getExtentOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Box2d();
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
