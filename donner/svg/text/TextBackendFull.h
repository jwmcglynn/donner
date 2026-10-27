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
  /// Construct a full text backend using the provided font manager and ECS registry.
  TextBackendFull(FontManager& fontManager, Registry& registry);
  ~TextBackendFull() override;

  // Non-copyable (owns HarfBuzz/FreeType resources).
  TextBackendFull(const TextBackendFull&) = delete;
  TextBackendFull& operator=(const TextBackendFull&) = delete;

  FontVMetrics fontVMetrics(FontHandle font) const override;
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const override;
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const override;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const override;
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle font) const override;
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const override;
  Path glyphOutline(FontHandle font, int glyphIndex, float scale) const override;
  bool isBitmapOnly(FontHandle font) const override;
  bool isCursive(uint32_t codepoint) const override;
  bool hasSmallCapsFeature(FontHandle font) const override;
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                         float scale) const override;
  std::optional<BitmapGlyphMetrics> bitmapGlyphMetrics(FontHandle font, int glyphIndex,
                                                       float scale) const override;
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex, float scale,
                                         const BitmapGlyphMetrics& admitted,
                                         std::size_t maximumRgbaBytes) const override;
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

  /// Whether the HarfBuzz/FreeType glyph-load flags suppress embedded bitmap strikes.
  ///
  /// This narrow test seam verifies that document-provided fonts cannot make HarfBuzz metric
  /// callbacks decode embedded bitmap strikes.
  [[nodiscard]] bool embeddedBitmapLoadingDisabledForTesting(FontHandle font) const;

  /// Total bytes of shaping context scanned to detect direction and script, a test seam that
  /// bounds the work shaping a chunk's ranges in context does.
  [[nodiscard]] std::size_t detectedContextBytesForTesting() const { return detectedContextBytes_; }

private:
  ShapedRun shapeRunImpl(FontHandle font, float fontSizePx, std::string_view spanText,
                         size_t byteOffset, size_t byteLength, bool isVertical,
                         TextOrientation textOrientation, FontVariant fontVariant,
                         bool enableKerning, bool forceLogicalOrder,
                         const ShapingContext* context = nullptr) const;
  /// Decode a previously validated current glyph slot without another metrics-only load.
  std::optional<BitmapGlyph> decodeBitmapGlyph(const BitmapGlyphMetrics& metrics) const;

  FontManager& fontManager_;
  Registry& registry_;
  /// See \ref detectedContextBytesForTesting.
  mutable std::size_t detectedContextBytes_ = 0;

  /// Cache entry stored on the font entity.
  struct HbFontEntry;

  /// Get or create a HarfBuzz font object for a FontHandle.
  hb_font_t* getOrCreateHbFont(FontHandle handle) const;
};

}  // namespace donner::svg
