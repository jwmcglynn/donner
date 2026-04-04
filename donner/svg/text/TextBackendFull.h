#pragma once
/// @file

#include "donner/svg/text/TextBackend.h"

struct hb_font_t;

namespace donner::svg {

/**
 * HarfBuzz + FreeType font backend.
 *
 * Provides full OpenType shaping (GSUB/GPOS), FreeType glyph outlines, cursive script
 * detection, native small-caps queries, and bitmap glyph extraction (CBDT/CBLC).
 */
class TextBackendFull final : public TextBackend {
public:
  TextBackendFull(FontManager& fontManager, Registry& registry);
  ~TextBackendFull() override;

  // Non-copyable (owns HarfBuzz/FreeType resources).
  TextBackendFull(const TextBackendFull&) = delete;
  TextBackendFull& operator=(const TextBackendFull&) = delete;

  FontVMetrics fontVMetrics(FontHandle font) const override;
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const override;
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const override;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const override;
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
  FontManager& fontManager_;
  Registry& registry_;

  /// Cache entry stored on the font entity.
  struct HbFontEntry;

  /// Get or create a HarfBuzz font object for a FontHandle.
  hb_font_t* getOrCreateHbFont(FontHandle handle) const;
};

}  // namespace donner::svg
