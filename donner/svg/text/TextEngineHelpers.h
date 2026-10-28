#pragma once
/// @file
/// Internal helpers for TextEngine layout. Exposed in a header for unit testing.

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/MathUtils.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg::text_engine_detail {

/// Map positive-size bitmap texels through their bearing and stretch, then rotate at glyph origin.
/// @param glyph Placed glyph in text-element local coordinates.
/// @param bitmap Decoded bitmap or metadata with positive width and height.
/// @return Transform from bitmap texels to text-element local coordinates.
template <typename Bitmap>
Transform2d computeLocalFromBitmapGlyph(const TextGlyph& glyph, const Bitmap& bitmap) {
  const double targetX = glyph.xPosition + bitmap.bearingX;
  const double targetY = glyph.yPosition - bitmap.bearingY;
  const double targetW = static_cast<double>(bitmap.width) * bitmap.scale * glyph.stretchScaleX;
  const double targetH = static_cast<double>(bitmap.height) * bitmap.scale * glyph.stretchScaleY;
  const double imgScaleX = targetW / static_cast<double>(bitmap.width);
  const double imgScaleY = targetH / static_cast<double>(bitmap.height);
  Transform2d localFromBitmap =
      Transform2d::Scale(imgScaleX, imgScaleY) * Transform2d::Translate(Vector2d(targetX, targetY));
  if (glyph.rotateDegrees != 0.0) {
    localFromBitmap =
        localFromBitmap * Transform2d::Translate(-glyph.xPosition, -glyph.yPosition) *
        Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
        Transform2d::Translate(glyph.xPosition, glyph.yPosition);
  }
  return localFromBitmap;
}

/// Compute the baseline-alignment offset for a `dominant-baseline` / `alignment-baseline`
/// keyword, in pixels. The offset is added to the baseline Y position, so positive values
/// move glyphs down (+Y in SVG coordinates); e.g. `hanging` returns +0.8 * ascent.
/// @param baseline Effective baseline keyword for the span.
/// @param vm Font vertical metrics in design units (`FontVMetrics::xHeight` may be 0).
/// @param scale Design units → pixels scale (em scale, fontSize / unitsPerEm).
double computeBaselineShift(DominantBaseline baseline, const FontVMetrics& vm, float scale);

/// Byte range within a span representing a shaping chunk.
struct ChunkRange {
  size_t byteStart = 0;  ///< Inclusive start byte offset within the span text.
  size_t byteEnd = 0;    ///< Exclusive end byte offset within the span text.
};

/// Pre-scan span text to find chunk byte boundaries. A new chunk starts when a non-first
/// typographic character has an absolute x or y position.
std::vector<ChunkRange> findChunkRanges(std::string_view spanText,
                                        const SmallVector<std::optional<Lengthd>, 1>& xList,
                                        const SmallVector<std::optional<Lengthd>, 1>& yList);

/// Index mapping result for a span's text.
struct ByteIndexMappings {
  std::vector<unsigned int> byteToCharIdx;  ///< Unicode codepoint index for SVG positioning.
};

/// Build the byte-to-codepoint map; shaping determines group applicability.
/// DOM indexing is separate.
ByteIndexMappings buildByteIndexMappings(std::string_view spanText);

/// Text chunk boundary for per-chunk text-anchor adjustment.
struct ChunkBoundary {
  size_t runIndex = 0;    ///< Index of the run that starts this chunk.
  size_t glyphIndex = 0;  ///< Glyph index within the run where the chunk begins.
  TextAnchor textAnchor = TextAnchor::Start;  ///< Effective text-anchor for the chunk.
  bool absoluteX = false;  ///< This boundary applies an authored absolute x position.
  bool absoluteY = false;  ///< This boundary applies an authored absolute y position.
};

/// Per-run pen start/end position for textLength calculation.
struct RunPenExtent {
  double startX = 0.0;  ///< Pen X position at the start of the run.
  double startY = 0.0;  ///< Pen Y position at the start of the run.
  double endX = 0.0;    ///< Pen X position at the end of the run.
  double endY = 0.0;    ///< Pen Y position at the end of the run.
};

/// Optional operation counts for deterministic textLength traversal complexity tests.
struct TextLengthTraversalStats {
  size_t runVisits = 0;             ///< Run visits in the per-span adjustment phase.
  size_t glyphVisits = 0;           ///< Glyph visits in the per-span adjustment phase.
  size_t inlinePositionVisits = 0;  ///< Inline-position entries inspected for resets.
  size_t mappedTextBytes = 0;       ///< Span text bytes mapped to SVG character indices.
};

/// Apply per-span and global textLength adjustments to positioned runs.
void applyTextLength(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents, const TextLayoutParams& params,
                     bool vertical, double currentPenX, double currentPenY,
                     TextLengthTraversalStats* traversalStats = nullptr);

/// Fix up chunk text-anchors and apply per-chunk text-anchor adjustment.
void applyTextAnchor(std::vector<TextRun>& runs, std::vector<ChunkBoundary>& chunkBoundaries,
                     const components::ComputedTextComponent& text, bool vertical);

/// Apply whitespace-dependent line layout along the inline axis, before text-anchor adjustment.
/// Each line/chunk uses its first addressable character's anchor; lines advance along the block
/// axis.
///
/// Soft wrapping uses spaces and tabs according to each span's white-space policy. Preserved
/// segment breaks force new lines even without inline-size. There is no mid-word breaking,
/// hyphenation, or CJK per-character breaking. Collapsible trailing spaces are removed;
/// pre-wrap spaces may hang, while break-spaces gives each preserved space a break opportunity.
/// Text-on-path runs are excluded: their controls are shaped as blanks without creating line boxes.
///
/// @param runs Runs produced by the single-line layout; glyph positions are rewritten.
/// @param text Computed text tree, parallel to \p runs (span i corresponds to run i).
/// @param params Layout params, including writing mode.
/// @param measurePx The inline-size measure in pixels, or zero to apply only forced breaks/tabs.
/// @param lineHeightPx Advance between line baselines along the block axis, in pixels.
/// @param backend Font metrics for the minimum tab advance (0.5ch).
/// @param chunks Authored text chunk boundaries, retained across line breaks.
/// @return true if multiple lines were laid out. Tab-only changes return false.
bool applyInlineSizeWrap(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                         const TextLayoutParams& params, double measurePx, double lineHeightPx,
                         const TextBackend* backend = nullptr,
                         std::span<const ChunkBoundary> chunks = {});

/// Compute per-span baseline-shift in pixels, including OS/2 sub/super metrics
/// and ancestor baseline-shift accumulation.
double computeSpanBaselineShiftPx(const TextBackend& backend,
                                  const components::ComputedTextComponent::TextSpan& span,
                                  FontHandle spanFont, float spanScale,
                                  const TextLayoutParams& params);

}  // namespace donner::svg::text_engine_detail
