#pragma once
/// @file
/// Internal helpers for TextEngine layout. Exposed in a header for unit testing.

#include <optional>
#include <string_view>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg::text_engine_detail {

/// Compute baseline shift for a given DominantBaseline/AlignmentBaseline value.
/// Returns a Y offset in pixels (positive = shift up toward ascender).
double computeBaselineShift(DominantBaseline baseline, const FontVMetrics& vm, float scale);

/// Byte range within a span representing a shaping chunk.
struct ChunkRange {
  size_t byteStart = 0;
  size_t byteEnd = 0;
};

/// Pre-scan span text to find chunk byte boundaries. A new chunk starts when a non-first
/// codepoint has an absolute x or y position.
std::vector<ChunkRange> findChunkRanges(std::string_view spanText,
                                        const SmallVector<std::optional<Lengthd>, 1>& xList,
                                        const SmallVector<std::optional<Lengthd>, 1>& yList);

/// Index mapping result for a span's text.
struct ByteIndexMappings {
  std::vector<unsigned int> byteToCharIdx;   ///< SVG addressable character index.
  std::vector<unsigned int> byteToRawCpIdx;  ///< Raw codepoint index (for rotation).
};

/// Build byte→charIdx and byte→rawCpIdx mappings for a span.
ByteIndexMappings buildByteIndexMappings(std::string_view spanText);

/// Text chunk boundary for per-chunk text-anchor adjustment.
struct ChunkBoundary {
  size_t runIndex = 0;
  size_t glyphIndex = 0;
  TextAnchor textAnchor = TextAnchor::Start;
};

/// Per-run pen start/end position for textLength calculation.
struct RunPenExtent {
  double startX = 0.0;
  double startY = 0.0;
  double endX = 0.0;
  double endY = 0.0;
};

/// Apply per-span and global textLength adjustments to positioned runs.
void applyTextLength(std::vector<TextRun>& runs,
                     const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents,
                     const TextLayoutParams& params, bool vertical, double currentPenX,
                     double currentPenY);

/// Fix up chunk text-anchors and apply per-chunk text-anchor adjustment.
void applyTextAnchor(std::vector<TextRun>& runs, std::vector<ChunkBoundary>& chunkBoundaries,
                     const components::ComputedTextComponent& text, bool vertical);

/// Compute per-span baseline-shift in pixels, including OS/2 sub/super metrics
/// and ancestor baseline-shift accumulation.
double computeSpanBaselineShiftPx(const TextBackend& backend,
                                  const components::ComputedTextComponent::TextSpan& span,
                                  FontHandle spanFont, float spanScale,
                                  const TextLayoutParams& params);

}  // namespace donner::svg::text_engine_detail
