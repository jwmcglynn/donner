#pragma once
/// @file

#include <vector>

#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/resources/FontManager.h"

struct hb_font_t;

namespace donner::svg {

/**
 * A single positioned glyph in a text run.
 *
 * Identical to TextLayout's LayoutGlyph — both shaping implementations produce the same output
 * format so renderers can consume either interchangeably.
 */
struct ShapedGlyph {
  int glyphIndex = 0;       ///< HarfBuzz glyph index (codepoint in HarfBuzz terms).
  double xPosition = 0;     ///< Absolute X position for this glyph.
  double yPosition = 0;     ///< Absolute Y baseline position.
  double xAdvance = 0;      ///< Horizontal advance to next glyph.
  double yAdvance = 0;      ///< Vertical advance (used in vertical writing modes).
  double rotateDegrees = 0; ///< Per-glyph rotation in degrees.
};

/**
 * A run of positioned glyphs sharing the same font, produced by HarfBuzz shaping.
 */
struct ShapedTextRun {
  FontHandle font;                    ///< Font handle for this run.
  std::vector<ShapedGlyph> glyphs;    ///< Positioned glyphs.
};

/**
 * HarfBuzz-based text shaper that replaces TextLayout when the `text_shaping` tier is enabled.
 *
 * Uses HarfBuzz for full OpenType shaping (GSUB/GPOS), producing the same output format as
 * TextLayout so renderers can consume either implementation identically. HarfBuzz handles:
 * - GPOS kerning (replaces stb_truetype's kern-table-only kerning)
 * - GSUB substitutions (ligatures like fi, fl, ffi)
 * - Contextual forms and mark positioning
 *
 * Glyph outlines are extracted via HarfBuzz's `hb_font_draw_glyph()` draw API (HarfBuzz 7.0+),
 * replacing stb_truetype's `stbtt_GetGlyphShape()`.
 */
class TextShaper {
public:
  explicit TextShaper(FontManager& fontManager);
  ~TextShaper();

  // Non-copyable (owns HarfBuzz resources).
  TextShaper(const TextShaper&) = delete;
  TextShaper& operator=(const TextShaper&) = delete;

  /**
   * Shape and lay out all spans in the computed text component, returning positioned glyph runs.
   *
   * Uses HarfBuzz for full OpenType shaping. The output format is identical to
   * TextLayout::layout() so renderers can consume either.
   *
   * @param text The computed text component with resolved span positions.
   * @param params Text parameters including font families and size.
   * @return A vector of shaped runs, one per span.
   */
  std::vector<ShapedTextRun> layout(const components::ComputedTextComponent& text,
                                    const TextParams& params);

  /**
   * Extract glyph outlines via HarfBuzz's draw API, producing a PathSpline.
   *
   * This replaces stb_truetype's `glyphToPathSpline()` when the text_shaping tier is enabled.
   *
   * @param font Font handle.
   * @param glyphIndex Glyph index to extract.
   * @param scale Scale factor for the desired font size.
   * @return The glyph outline as a PathSpline, or an empty spline if extraction fails.
   */
  PathSpline glyphOutline(FontHandle font, int glyphIndex, float scale);

  /**
   * Bitmap glyph data extracted from a color font (e.g., CBDT emoji).
   */
  struct BitmapGlyph {
    std::vector<uint8_t> rgbaPixels;  ///< RGBA pixel data (premultiplied alpha).
    int width = 0;                    ///< Bitmap width in pixels.
    int height = 0;                   ///< Bitmap height in pixels.
    double bearingX = 0;              ///< Horizontal offset from glyph origin.
    double bearingY = 0;              ///< Vertical offset from baseline (positive = up).
    double scale = 1.0;               ///< Scale factor to apply (bitmap ppem vs requested size).
  };

  /**
   * Extract a bitmap glyph from a color font (CBDT/CBLC).
   *
   * @param font Font handle.
   * @param glyphIndex Glyph index to extract.
   * @param requestedScale The fontSizePx / upem scale for the desired size.
   * @return The bitmap glyph data, or std::nullopt if the glyph is not a bitmap.
   */
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex, float requestedScale);

private:
  FontManager& fontManager_;

  /// Cache of HarfBuzz font objects, keyed by FontHandle index.
  struct HbFontEntry;
  std::vector<std::unique_ptr<HbFontEntry>> hbFonts_;

  /// Get or create a HarfBuzz font object for a FontHandle.
  hb_font_t* getHbFont(FontHandle handle);
};

}  // namespace donner::svg
