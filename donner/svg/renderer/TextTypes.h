#pragma once
/// @file

#include <vector>

#include "donner/svg/resources/FontManager.h"

namespace donner::svg {

/**
 * A single positioned glyph in a laid-out text run.
 *
 * Produced by both TextLayout (simple backend) and TextShaper (full backend).
 */
struct TextGlyph {
  int glyphIndex = 0;          ///< Glyph index (backend-specific: stbtt or HarfBuzz).
  double xPosition = 0;        ///< Absolute X position for this glyph.
  double yPosition = 0;        ///< Absolute Y baseline position.
  double xAdvance = 0;         ///< Horizontal advance to next glyph.
  double yAdvance = 0;         ///< Vertical advance (used in vertical writing modes).
  double rotateDegrees = 0;    ///< Per-glyph rotation in degrees.
  float fontSizeScale = 1.0f;  ///< Per-glyph font size multiplier (< 1.0 for small-caps).
};

/**
 * A run of positioned glyphs sharing the same font.
 */
struct TextRun {
  FontHandle font;               ///< Font handle for this run.
  std::vector<TextGlyph> glyphs; ///< Positioned glyphs.
};

}  // namespace donner::svg
