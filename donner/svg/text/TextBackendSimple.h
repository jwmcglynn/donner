#pragma once
/// @file

/// Declares stb_truetype functions with external linkage so they can be defined
/// exactly once in the matching .cc file.
#define STBTT_DEF extern
#include <stb/stb_truetype.h>

#include "donner/svg/text/TextBackend.h"

namespace donner::svg {

/**
 * stb_truetype-based font backend.
 *
 * Provides font metrics, glyph outlines, and table-derived data using stb_truetype.
 * No GSUB/GPOS shaping beyond native small caps (the face's `smcp` single substitutions), no
 * bitmap glyph support, no cursive script detection.
 */
class TextBackendSimple final : public TextBackend {
public:
  /// Construct a simple text backend using the provided font manager and ECS registry.
  TextBackendSimple(FontManager& fontManager, Registry& registry);

  bool requiresTrustedFontData() const override { return true; }

  FontVMetrics fontVMetrics(FontHandle font) const override;
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const override;
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const override;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const override;
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle font) const override;
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const override;
  Path glyphOutline(FontHandle font, int glyphIndex, float scale) const override;
  Path glyphOutline(FontHandle font, int glyphIndex, float scale,
                    bool allowSyntheticOblique) const override;
  bool isBitmapOnly(FontHandle font) const override;
  bool isCursive(uint32_t codepoint) const override;
  bool hasSmallCapsFeature(FontHandle font) const override;
  bool hasSmallCapsFeatureForRun(FontHandle font, std::string_view text, size_t offset,
                                 size_t length, bool vertical, TextOrientation orientation,
                                 const ShapingContext* context) const override;
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                         float scale) const override;
  ShapedRun shapeRun(FontHandle font, float fontSizePx, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool isVertical, FontVariant fontVariant,
                     bool forceLogicalOrder) const override;
  ShapedRun shapeRun(FontHandle font, float fontSizePx, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool isVertical,
                     TextOrientation textOrientation, FontVariant fontVariant,
                     bool forceLogicalOrder) const override;
  ShapedRun shapeRunNoKerning(FontHandle font, float fontSizePx, std::string_view spanText,
                              size_t byteOffset, size_t byteLength, bool isVertical,
                              FontVariant fontVariant, bool forceLogicalOrder) const override;
  ShapedRun shapeRunNoKerning(FontHandle font, float fontSizePx, std::string_view spanText,
                              size_t byteOffset, size_t byteLength, bool isVertical,
                              TextOrientation textOrientation, FontVariant fontVariant,
                              bool forceLogicalOrder) const override;
  ShapedRun shapeRunInContext(FontHandle font, float fontSizePx, std::string_view spanText,
                              const ShapingContext& context, size_t byteOffset, size_t byteLength,
                              bool isVertical, TextOrientation textOrientation,
                              FontVariant fontVariant, bool kerning) const override;
  double crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont, float curSizePx,
                       uint32_t prevCodepoint, uint32_t curCodepoint,
                       bool isVertical) const override;
  double crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont, float curSizePx,
                       uint32_t prevCodepoint, uint32_t curCodepoint, bool isVertical,
                       TextOrientation textOrientation) const override;

  double crossSpanKern(FontHandle previousFont, float previousSize, FontHandle currentFont,
                       float currentSize, uint32_t previousCodepoint, uint32_t currentCodepoint,
                       bool vertical, TextOrientation orientation,
                       const ShapingContext& context) const override;

private:
  ShapedRun shapeRunImpl(FontHandle font, float fontSizePx, std::string_view spanText,
                         size_t byteOffset, size_t byteLength, bool isVertical,
                         TextOrientation textOrientation, FontVariant fontVariant,
                         bool enableKerning, bool forceLogicalOrder, bool backward = false,
                         const ShapingContext* context = nullptr) const;

  const stbtt_fontinfo* getFontInfo(FontHandle font) const;

  FontManager& fontManager_;
  Registry& registry_;
};

}  // namespace donner::svg
