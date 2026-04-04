#include "donner/svg/renderer/TextEngine.h"

#include <cmath>
#include <string>

#include "donner/base/MathUtils.h"
#include "donner/svg/core/WritingMode.h"

namespace donner::svg {

namespace {

// TODO: Switch to donner/base/Utf8.h instead of reimplementing UTF-8 decoding.

/// Decode a single UTF-8 codepoint, advancing \p i past the consumed bytes.
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
    const uint32_t cp =
        (static_cast<uint32_t>(byte & 0x1F) << 6) | (static_cast<uint32_t>(str[i + 1]) & 0x3F);
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
  if (cp <= 0x7F) {
    result.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }

  return result;
}

/// Returns true if \p font shapes \p cp to a non-.notdef glyph.
bool fontSupportsCodepoint(TextBackend& backend, FontHandle font, float fontSizePx, uint32_t cp) {
  if (!font || cp == 0) {
    return false;
  }

  const std::string utf8 = encodeUtf8(cp);
  const auto shaped =
      backend.shapeRun(font, fontSizePx, utf8, 0, utf8.size(), false, FontVariant::Normal, false);
  return !shaped.glyphs.empty() && shaped.glyphs.front().glyphIndex != 0;
}

/// Finds a registered font that covers \p cp, preserving \p currentFont when it already does.
FontHandle findCoverageFallbackFont(TextBackend& backend, FontManager& fontManager,
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

}  // namespace

TextEngine::TextEngine(TextBackend& backend, FontManager& fontManager)
    : backend_(backend), fontManager_(fontManager) {}

std::vector<TextRun> TextEngine::layout(const components::ComputedTextComponent& text,
                                        const TextParams& params) {
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
  // The shift moves the alphabetic baseline so that the dominant baseline sits at y.
  const float scale = backend_.scaleForPixelHeight(font, fontSizePx);
  double baselineShift = 0.0;
  if (params.dominantBaseline != DominantBaseline::Auto &&
      params.dominantBaseline != DominantBaseline::Alphabetic) {
    const FontVMetrics vm = backend_.fontVMetrics(font);
    // ascent > 0 (above baseline), descent < 0 (below baseline), in font units.
    switch (params.dominantBaseline) {
      case DominantBaseline::Auto:
      case DominantBaseline::Alphabetic: break;
      case DominantBaseline::Middle:
      case DominantBaseline::Central:
        // Center of the em box: (ascent + descent) / 2 above the alphabetic baseline.
        baselineShift = static_cast<double>(vm.ascent + vm.descent) * 0.5 * scale;
        break;
      case DominantBaseline::Hanging:
        // Hanging baseline: approximately 80% of ascent above alphabetic.
        baselineShift = static_cast<double>(vm.ascent) * 0.8 * scale;
        break;
      case DominantBaseline::Mathematical:
        // Mathematical baseline: approximately 50% of ascent above alphabetic.
        baselineShift = static_cast<double>(vm.ascent) * 0.5 * scale;
        break;
      case DominantBaseline::TextTop:
        // Top of em box: ascent above alphabetic.
        baselineShift = static_cast<double>(vm.ascent) * scale;
        break;
      case DominantBaseline::TextBottom:
      case DominantBaseline::Ideographic:
        // Bottom of em box / ideographic baseline: descent below alphabetic.
        baselineShift = static_cast<double>(vm.descent) * scale;
        break;
    }
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

  // Text chunk boundaries for per-chunk text-anchor adjustment.
  // A new chunk starts at the first glyph, at any span with startsNewChunk=true,
  // and at any character with an absolute x or y position within a span.
  struct ChunkBoundary {
    size_t runIndex = 0;    // Index into runs vector.
    size_t glyphIndex = 0;  // Index into the run's glyph vector.
    TextAnchor textAnchor = TextAnchor::Start;
  };
  std::vector<ChunkBoundary> chunkBoundaries;

  // Per-run pen start position, tracked for per-span textLength calculation.
  struct RunPenExtent {
    double startX = 0.0;
    double startY = 0.0;
    double endX = 0.0;
    double endY = 0.0;
  };
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
    spanFont = findCoverageFallbackFont(backend_, fontManager_, spanFont, spanFontSizePx,
                                        spanTestCodepoint);
    run.font = spanFont;

    // Note: bitmap-only fonts (e.g., color emoji) are valid for the full backend.
    // The simple backend can't handle them but produces empty shapeRun results,
    // which the renderer gracefully skips.

    const float spanScale = backend_.scaleForEmToPixels(spanFont, spanFontSizePx);

    // ── Per-span baseline-shift ─────────────────────────────────────────────────
    // Positive CSS baseline-shift = shift up, which in SVG Y-down = subtract from Y.
    // For sub/super keywords, prefer font OS/2 metrics over hardcoded em constants.
    FontMetrics spanFontMetrics = params.fontMetrics;
    spanFontMetrics.fontSize = spanFontSizePx;
    double spanBaselineShiftPx;
    using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
    if (span.baselineShiftKeyword == BSK::Sub || span.baselineShiftKeyword == BSK::Super) {
      const auto subSuper = backend_.subSuperMetrics(spanFont);
      if (subSuper.has_value()) {
        // OS/2 offsets are in font design units (positive = up). Convert to pixels and
        // negate for CSS convention (positive CSS baseline-shift = shift up = negative Y).
        if (span.baselineShiftKeyword == BSK::Sub) {
          // Subscript: ySubscriptYOffset measures distance below baseline (positive value).
          // CSS sub shifts DOWN, so the CSS value is negative.
          spanBaselineShiftPx = -static_cast<double>(subSuper->subscriptYOffset) * spanScale;
        } else {
          // Superscript: ySuperscriptYOffset measures distance above baseline (positive value).
          // CSS super shifts UP, so the CSS value is positive.
          spanBaselineShiftPx = static_cast<double>(subSuper->superscriptYOffset) * spanScale;
        }
      } else {
        // Fallback to em-based constants if OS/2 table is missing.
        spanBaselineShiftPx =
            span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
      }
    } else {
      spanBaselineShiftPx =
          span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
    }

    // Resolve ancestor baseline-shifts, using font OS/2 metrics for sub/super keywords.
    {
      const auto subSuper = backend_.subSuperMetrics(spanFont);
      for (const auto& ancestor : span.ancestorBaselineShifts) {
        if (subSuper.has_value() && ancestor.keyword == BSK::Sub) {
          const float ancestorScale =
              backend_.scaleForEmToPixels(spanFont, static_cast<float>(ancestor.fontSizePx));
          spanBaselineShiftPx += -static_cast<double>(subSuper->subscriptYOffset) * ancestorScale;
        } else if (subSuper.has_value() && ancestor.keyword == BSK::Super) {
          const float ancestorScale =
              backend_.scaleForEmToPixels(spanFont, static_cast<float>(ancestor.fontSizePx));
          spanBaselineShiftPx += static_cast<double>(subSuper->superscriptYOffset) * ancestorScale;
        } else {
          FontMetrics ancestorFm = params.fontMetrics;
          ancestorFm.fontSize = ancestor.fontSizePx;
          spanBaselineShiftPx +=
              ancestor.shift.toPixels(params.viewBox, ancestorFm, Lengthd::Extent::Y);
        }
      }
    }

    // ── Alignment-baseline override ─────────────────────────────────────────────
    // If alignment-baseline is set on this span, it overrides the text-level dominant-baseline.
    double effectiveBaselineShift = baselineShift;
    if (span.alignmentBaseline != DominantBaseline::Auto) {
      effectiveBaselineShift = 0.0;
      const FontVMetrics vm = backend_.fontVMetrics(spanFont);
      switch (span.alignmentBaseline) {
        case DominantBaseline::Auto:
        case DominantBaseline::Alphabetic: break;
        case DominantBaseline::Middle:
        case DominantBaseline::Central:
          effectiveBaselineShift = static_cast<double>(vm.ascent + vm.descent) * 0.5 * spanScale;
          break;
        case DominantBaseline::Hanging:
          effectiveBaselineShift = static_cast<double>(vm.ascent) * 0.8 * spanScale;
          break;
        case DominantBaseline::Mathematical:
          effectiveBaselineShift = static_cast<double>(vm.ascent) * 0.5 * spanScale;
          break;
        case DominantBaseline::TextTop:
          effectiveBaselineShift = static_cast<double>(vm.ascent) * spanScale;
          break;
        case DominantBaseline::TextBottom:
        case DominantBaseline::Ideographic:
          effectiveBaselineShift = static_cast<double>(vm.descent) * spanScale;
          break;
      }
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
    // A chunk boundary occurs when a non-first codepoint has an absolute x or y
    // position. We shape each chunk separately so that the HarfBuzz backend
    // (where kerning is baked into advances) doesn't produce incorrect kerning
    // across chunk boundaries.
    struct ChunkRange {
      size_t byteStart = 0;
      size_t byteEnd = 0;
    };
    std::vector<ChunkRange> chunkRanges;
    {
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

        // Check if this character has absolute positioning.
        bool hasAbsX = scanCharIdx < xListLocal.size() && xListLocal[scanCharIdx].has_value();
        bool hasAbsY = scanCharIdx < yListLocal.size() && yListLocal[scanCharIdx].has_value();

        if ((hasAbsX || hasAbsY) && byteStart != currentChunkStart) {
          // Start a new chunk at this character.
          chunkRanges.push_back({currentChunkStart, byteStart});
          currentChunkStart = byteStart;
        }
      }
      // Final chunk.
      chunkRanges.push_back({currentChunkStart, spanText.size()});
    }

    // ── Build byte→index mappings for the span text ──────────────────────────────
    // byteToCharIdx: SVG addressable character index (combining marks share base index).
    // byteToRawCpIdx: Raw codepoint index (every codepoint gets its own index, for rotation).
    std::vector<unsigned int> byteToCharIdx(spanText.size(), 0);
    std::vector<unsigned int> byteToRawCpIdx(spanText.size(), 0);
    {
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
        // Fill all bytes of this codepoint with the same indices.
        for (size_t j = startBi; j < bi && j < spanText.size(); ++j) {
          byteToCharIdx[j] = ci;
          byteToRawCpIdx[j] = rawCi;
        }
      }
    }

    // ── Shape each chunk and iterate its glyphs ─────────────────────────────────
    uint32_t lastCodepoint = 0;

    // Track last codepoint per chunk for cross-chunk kerning.
    uint32_t prevChunkLastCodepoint = prevSpanLastCodepoint;
    FontHandle prevChunkFont = prevSpanFont;
    float prevChunkFontSizePx = prevSpanFontSizePx;

    for (size_t ci = 0; ci < chunkRanges.size(); ++ci) {
      const auto& chunk = chunkRanges[ci];
      const auto shaped =
          backend_.shapeRun(spanFont, spanFontSizePx, spanText, chunk.byteStart,
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
            backend_.crossSpanKern(prevChunkFont, prevChunkFontSizePx, spanFont, spanFontSizePx,
                                   prevChunkLastCodepoint, firstCp, vertical);
        appliedCrossKern = true;
      } else if (ci == 0 && !span.startsNewChunk && prevSpanLastCodepoint != 0 &&
                 !shaped.glyphs.empty()) {
        // Cross-span kerning for the first chunk.
        size_t firstByteIdx = chunk.byteStart;
        const uint32_t firstCp = decodeUtf8(spanText, firstByteIdx);
        crossKern =
            backend_.crossSpanKern(prevSpanFont, prevSpanFontSizePx, spanFont, spanFontSizePx,
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
            const FontVMetrics vm = backend_.fontVMetrics(spanFont);
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
          if (!backend_.isCursive(codepoint)) {
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
          if (!backend_.isCursive(codepoint)) {
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
  // textLength can be specified per-span (on tspan elements) or globally (on the text element).
  // Per-span textLength takes priority.
  if (!runs.empty()) {
    const bool vertical = isVertical(params.writingMode);

    // Check if any span has per-span textLength.
    bool anySpanHasTextLength = false;
    for (const auto& span : text.spans) {
      if (span.textLength.has_value()) {
        anySpanHasTextLength = true;
        break;
      }
    }

    // ── Apply per-span textLength to individual runs ────────────────────────────
    if (anySpanHasTextLength) {
      for (size_t i = 0; i < runs.size() && i < text.spans.size(); ++i) {
        auto& run = runs[i];
        const auto& span = text.spans[i];
        if (!span.textLength.has_value() || run.glyphs.empty()) {
          continue;
        }

        const double runStartPos = vertical ? run.glyphs[0].yPosition : run.glyphs[0].xPosition;
        // Natural length from pen tracking (includes kerning, letter-spacing, word-spacing).
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

    // ── Global textLength ───────────────────────────────────────────────────────
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

    // Apply global textLength only when no span has per-span textLength.
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

    // ── Fix up chunk text-anchors ───────────────────────────────────────────────
    // For each chunk, use the text-anchor from the first span that has actual glyph content
    // (skipping empty whitespace-only spans).
    for (auto& chunk : chunkBoundaries) {
      for (size_t ri = chunk.runIndex; ri < runs.size(); ++ri) {
        const size_t gStart = (ri == chunk.runIndex) ? chunk.glyphIndex : 0;
        if (gStart < runs[ri].glyphs.size()) {
          chunk.textAnchor = text.spans[ri].textAnchor;
          break;
        }
      }
    }

    // ── Apply text-anchor adjustment per text chunk ─────────────────────────────
    for (size_t ci = 0; ci < chunkBoundaries.size(); ++ci) {
      const auto& chunk = chunkBoundaries[ci];
      if (chunk.textAnchor == TextAnchor::Start) {
        continue;
      }

      // Determine the end boundary (next chunk or end of all runs).
      size_t endRunIdx = runs.size();
      size_t endGlyphIdx = 0;
      if (ci + 1 < chunkBoundaries.size()) {
        endRunIdx = chunkBoundaries[ci + 1].runIndex;
        endGlyphIdx = chunkBoundaries[ci + 1].glyphIndex;
      }

      // Compute chunk extent: first glyph position to last glyph end position.
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

  return runs;
}

}  // namespace donner::svg
