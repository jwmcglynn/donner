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

/// Compute the baseline-alignment offset for a `dominant-baseline` / `alignment-baseline`
/// keyword, in pixels. The offset is added to the baseline Y position, so positive values
/// move glyphs down (+Y in SVG coordinates); e.g. `hanging` returns +0.8 * ascent.
/// @param baseline Effective baseline keyword for the span.
/// @param vm Font vertical metrics in design units (\ref FontVMetrics::xHeight may be 0).
/// @param scale Design units → pixels scale (em scale, fontSize / unitsPerEm).
double computeBaselineShift(DominantBaseline baseline, const FontVMetrics& vm, float scale);

/// Byte range within a span representing a shaping chunk.
struct ChunkRange {
  size_t byteStart = 0;  ///< Inclusive start byte offset within the span text.
  size_t byteEnd = 0;    ///< Exclusive end byte offset within the span text.
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
  size_t runIndex = 0;    ///< Index of the run that starts this chunk.
  size_t glyphIndex = 0;  ///< Glyph index within the run where the chunk begins.
  TextAnchor textAnchor = TextAnchor::Start;  ///< Effective text-anchor for the chunk.
};

/// Per-run pen start/end position for textLength calculation.
struct RunPenExtent {
  double startX = 0.0;  ///< Pen X position at the start of the run.
  double startY = 0.0;  ///< Pen Y position at the start of the run.
  double endX = 0.0;    ///< Pen X position at the end of the run.
  double endY = 0.0;    ///< Pen Y position at the end of the run.
};

/// Apply per-span and global textLength adjustments to positioned runs.
void applyTextLength(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents, const TextLayoutParams& params,
                     bool vertical, double currentPenX, double currentPenY);

/// Fix up chunk text-anchors and apply per-chunk text-anchor adjustment.
void applyTextAnchor(std::vector<TextRun>& runs, std::vector<ChunkBoundary>& chunkBoundaries,
                     const components::ComputedTextComponent& text, bool vertical);

/// Break horizontally-laid-out runs into stacked lines for SVG2 `inline-size` auto-flow, rewriting
/// glyph positions in place. Called after the flat single-line layout and before text-anchor
/// adjustment; when it wraps it applies text-anchor per line itself.
///
/// Wrap rules (slice 1, horizontal writing modes only):
///  - Soft-wrap opportunities are whitespace (U+0020/tab/newline) between words; there is no
///    mid-word breaking, hyphenation, or CJK per-character breaking.
///  - Greedy line filling: a word is placed on the current line when it fits within \p measurePx,
///    otherwise it starts a new line. The first word of a line is always placed even when it
///    overflows the measure.
///  - Whitespace at a soft-wrap break hangs on the previous line and is not counted toward the
///    next line's width; intra-line spacing is preserved from the original layout.
///  - Each line is independently anchored by `text-anchor` (from \p params) about the block origin
///    (the first glyph's position) and stacked downward by \p lineHeightPx.
///
/// @param runs Runs produced by the flat horizontal layout; glyph positions are rewritten.
/// @param text Computed text tree, parallel to \p runs (span i ↔ run i), used for span text.
/// @param params Layout params; `textAnchor` selects per-line anchoring.
/// @param measurePx The inline-size measure in pixels (must be > 0).
/// @param lineHeightPx Vertical advance between line baselines in pixels.
/// @return true if wrapping was applied.
bool applyInlineSizeWrap(std::vector<TextRun>& runs,
                         const components::ComputedTextComponent& text,
                         const TextLayoutParams& params, double measurePx, double lineHeightPx);

/// Compute per-span baseline-shift in pixels, including OS/2 sub/super metrics
/// and ancestor baseline-shift accumulation.
double computeSpanBaselineShiftPx(const TextBackend& backend,
                                  const components::ComputedTextComponent::TextSpan& span,
                                  FontHandle spanFont, float spanScale,
                                  const TextLayoutParams& params);

}  // namespace donner::svg::text_engine_detail
