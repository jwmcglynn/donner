#pragma once
/// @file
/// Deterministic text scenes for exact text-decoration renderer comparisons.

#include <optional>
#include <string_view>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/text/TextBackend.h"

namespace donner::svg::tests {

/**
 * Text backend with fixed metrics and one-pixel-wide rectangular glyphs.
 *
 * Every character shapes to glyph 3 with a 10px advance. The glyph outline covers x in [0, 1] and
 * y in [-glyphHeight, 0] regardless of the font size, so a decorated scene can be reproduced
 * exactly with `<rect>` elements. At font-size 20 the metrics place the underline 2px below the
 * baseline, the overline 16px above it, and the line-through 6px above it, each 1px thick by
 * default.
 */
class DecorationBackend : public TextBackend {
public:
  /// @param underlineMetrics Whether the font reports underline metrics.
  /// @param glyphHeight Height of every glyph outline above the baseline, in pixels.
  explicit DecorationBackend(bool underlineMetrics = true, double glyphHeight = 2.0);

  FontVMetrics fontVMetrics(FontHandle) const override;
  float scaleForPixelHeight(FontHandle, float size) const override;
  float scaleForEmToPixels(FontHandle, float size) const override;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle) const override;
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle) const override;
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle) const override;
  Path glyphOutline(FontHandle, int, float) const override;
  bool isBitmapOnly(FontHandle) const override;
  bool isCursive(uint32_t) const override;
  bool hasSmallCapsFeature(FontHandle) const override;
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle, int, float) const override;
  ShapedRun shapeRun(FontHandle, float, std::string_view text, size_t begin, size_t length, bool,
                     FontVariant, bool) const override;
  double crossSpanKern(FontHandle, float, FontHandle, float, uint32_t, uint32_t,
                       bool) const override;

private:
  bool hasUnderlineMetrics_;
  double glyphHeight_;
};

/// Parse `body` into a 100x100 document whose text engine uses \ref DecorationBackend.
SVGDocument DecorationDocument(std::string_view body, bool metrics = true,
                               double glyphHeight = 2.0);

/// Render `body` with the active backend, using \ref DecorationBackend for text.
RendererBitmap RenderDecorationScene(std::string_view body, bool metrics = true,
                                     double glyphHeight = 2.0);

}  // namespace donner::svg::tests
