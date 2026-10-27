#pragma once
/// @file

#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Path.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {

/// Lifetime charge for retained text-path measurement geometry.
class TextPathMeasurementReservation {
public:
  /// Reserve geometry bytes from a document family, returning null on refusal.
  static std::shared_ptr<TextPathMeasurementReservation> Create(
      std::shared_ptr<components::DocumentResourceFamilyBudget> family, std::size_t bytes) {
    if (!family ||
        !family->reserve(components::DocumentResourceFamilyBudget::Kind::Geometry, bytes)) {
      return nullptr;
    }
    return std::shared_ptr<TextPathMeasurementReservation>(
        new TextPathMeasurementReservation(std::move(family), bytes));
  }

  ~TextPathMeasurementReservation() {
    family_->release(components::DocumentResourceFamilyBudget::Kind::Geometry, bytes_);
  }

  TextPathMeasurementReservation(const TextPathMeasurementReservation&) = delete;
  TextPathMeasurementReservation& operator=(const TextPathMeasurementReservation&) = delete;

  /// Reduce the retained charge after measurement determines exact capacities.
  void shrink(std::size_t bytes) {
    if (bytes < bytes_) {
      family_->release(components::DocumentResourceFamilyBudget::Kind::Geometry, bytes_ - bytes);
      bytes_ = bytes;
    }
  }

private:
  TextPathMeasurementReservation(std::shared_ptr<components::DocumentResourceFamilyBudget> family,
                                 std::size_t bytes)
      : family_(std::move(family)), bytes_(bytes) {}

  std::shared_ptr<components::DocumentResourceFamilyBudget> family_;
  std::size_t bytes_ = 0;
};

/// Shared path mapping used to warp glyph outlines for `textPath method="stretch"`.
struct StretchedTextPath {
  Path::MeasuredPath measuredPath;
  std::vector<double> segmentEndDistances;
  double startOffset = 0.0;
  bool closed = false;
  bool linear = false;
  std::shared_ptr<TextPathMeasurementReservation> measurementReservation;
};

/// Resolved face and used font size for layout or declaring-element decoration metrics.
struct ResolvedTextFont {
  FontHandle font;
  float usedSizePx = 0.0f;
};

/**
 * A single positioned glyph in a laid-out text run.
 *
 * Produced by the text engine for simple and full shaping backends.
 */
struct TextGlyph {
  int glyphIndex = 0;        ///< Glyph index (backend-specific: stbtt or HarfBuzz).
  double xPosition = 0;      ///< Absolute X position for this glyph.
  double yPosition = 0;      ///< Absolute Y baseline position.
  double xAdvance = 0;       ///< Horizontal advance to next glyph.
  double yAdvance = 0;       ///< Vertical advance (used in vertical writing modes).
  double xKern = 0;          ///< Kerning adjustment applied before this glyph (for path layout).
  double rotateDegrees = 0;  ///< Per-glyph rotation in degrees.
  uint32_t cluster = 0;      ///< Byte offset into the span text for the glyph's source cluster.
  /// Byte offset of the SVG positioning group; absent uses the source cluster.
  std::optional<uint32_t> positioningCluster;
  float fontSizeScale = 1.0f;            ///< Per-glyph font size multiplier (< 1.0 for small-caps).
  float stretchScaleX = 1.0f;            ///< Additional X-axis glyph stretch from lengthAdjust.
  float stretchScaleY = 1.0f;            ///< Additional Y-axis glyph stretch from lengthAdjust.
  std::optional<double> textPathOrigin;  ///< Logical path distance for a stretched outline origin.
  double textPathNormalOffset = 0.0;     ///< Offset from the path along its left normal.
  std::optional<double> textPathLocalRotateDegrees;  ///< Authored rotation before path warping.
  /// True when vertical layout set this glyph sideways, a quarter turn from upright, so the
  /// glyph's own x axis runs along the inline axis.
  bool sideways = false;
  /// Face for a cluster that the run's face cannot render; null when \ref TextRun::font renders
  /// it. Use \ref GlyphFont to resolve the face that draws the glyph.
  FontHandle font;
};

/**
 * A run of positioned glyphs laid out with one primary face. Clusters that face cannot render
 * carry their fallback face in \ref TextGlyph::font.
 */
struct TextRun {
  FontHandle font;                ///< Primary font handle for this run.
  float usedFontSizePx = 0.0f;    ///< Font size used for shaping, including font-size-adjust.
  std::vector<TextGlyph> glyphs;  ///< Positioned glyphs.
  bool onPath = false;            ///< True if glyphs are positioned along a textPath.
  bool textLengthAppliedOnPath =
      false;  ///< True when path placement consumed this run's textLength.
  std::shared_ptr<const StretchedTextPath> stretchedTextPath;  ///< Shared nonlinear path mapping.
};

/**
 * Returns the face that draws \p glyph: its fallback face when it has one, otherwise the run's.
 *
 * @param run Run that contains \p glyph.
 * @param glyph Glyph to resolve.
 */
inline FontHandle GlyphFont(const TextRun& run, const TextGlyph& glyph) {
  return glyph.font ? glyph.font : run.font;
}

}  // namespace donner::svg
