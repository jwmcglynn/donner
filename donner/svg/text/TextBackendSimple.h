#pragma once
/// @file

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

#include "donner/svg/text/TextBackend.h"

namespace donner::svg {

/**
 * stb_truetype-based font backend.
 *
 * Provides font metrics, glyph outlines, and table-derived data using stb_truetype.
 * No GSUB/GPOS shaping, no bitmap glyph support, no cursive script detection.
 */
class TextBackendSimple final : public TextBackend {
public:
  TextBackendSimple(FontManager& fontManager, Registry& registry);

  FontVMetrics fontVMetrics(FontHandle font) const override;
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const override;
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const override;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const override;
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle font) const override;
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const override;
  PathSpline glyphOutline(FontHandle font, int glyphIndex, float scale) const override;
  bool isBitmapOnly(FontHandle font) const override;
  bool isCursive(uint32_t codepoint) const override;
  bool hasSmallCapsFeature(FontHandle font) const override;
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                         float scale) const override;
  ShapedRun shapeRun(FontHandle font, float fontSizePx, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool isVertical, FontVariant fontVariant,
                     bool forceLogicalOrder) const override;
  double crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont, float curSizePx,
                       uint32_t prevCodepoint, uint32_t curCodepoint,
                       bool isVertical) const override;

private:
  const stbtt_fontinfo* getFontInfo(FontHandle font) const;

  FontManager& fontManager_;
  Registry& registry_;
};

}  // namespace donner::svg
