#pragma once
/// @file

#include <vector>

#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {

/**
 * A single positioned glyph in a text run.
 */
struct LayoutGlyph {
  int glyphIndex = 0;   ///< stb_truetype glyph index.
  double xPosition = 0; ///< Absolute X position for this glyph.
  double yPosition = 0; ///< Absolute Y baseline position.
  double xAdvance = 0;  ///< Advance width to next glyph (for reference).
  double rotateDegrees = 0; ///< Per-glyph rotation in degrees.
};

/**
 * A run of positioned glyphs sharing the same font.
 */
struct LayoutTextRun {
  FontHandle font;                  ///< Font handle for this run.
  std::vector<LayoutGlyph> glyphs;  ///< Positioned glyphs.
};

/**
 * Lays out text from `ComputedTextComponent` spans into positioned glyph sequences.
 *
 * Uses stb_truetype for codepoint→glyph mapping, advance widths, and `kern`-table kerning.
 * Produces `LayoutTextRun` arrays that can be consumed by any renderer backend.
 *
 * Designed to be replaceable by a HarfBuzz-based `TextShaper` in the `text_shaping` tier,
 * which produces the same `LayoutTextRun` output format.
 */
class TextLayout {
public:
  explicit TextLayout(FontManager& fontManager);

  /**
   * Lay out all spans in the computed text component, returning positioned glyph runs.
   *
   * @param text The computed text component with resolved span positions.
   * @param params Text parameters including font families and size.
   * @return A vector of layout runs, one per span.
   */
  std::vector<LayoutTextRun> layout(const components::ComputedTextComponent& text,
                                    const TextParams& params);

private:
  FontManager& fontManager_;
};

/**
 * Convert a glyph outline from stb_truetype into a `PathSpline`.
 *
 * @param info The stb_truetype font info.
 * @param glyphIndex The glyph index to extract.
 * @param scale Scale factor from `stbtt_ScaleForPixelHeight()`.
 * @return The glyph outline as a PathSpline, or an empty spline if extraction fails.
 */
PathSpline glyphToPathSpline(const stbtt_fontinfo* info, int glyphIndex, float scale);

}  // namespace donner::svg
