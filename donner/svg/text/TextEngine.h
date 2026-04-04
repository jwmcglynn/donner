#pragma once
/// @file

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "donner/css/FontFace.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

/**
 * Shared SVG text engine.
 *
 * Owns the selected text backend and provides layout plus font-related operations while hiding
 * backend selection from callers.
 *
 * Usage:
 * @code
 *   FontManager fontManager;
 *   TextEngine engine(fontManager);
 *   std::vector<TextRun> runs = engine.layout(text, params);
 * @endcode
 */
class TextEngine {
public:
  explicit TextEngine(FontManager& fontManager);
  ~TextEngine();

  TextEngine(const TextEngine&) = delete;
  TextEngine& operator=(const TextEngine&) = delete;

  void addFontFace(const css::FontFace& face);
  void addFontFaces(std::span<const css::FontFace> faces);

  /// Lay out all spans, returning positioned glyph runs.
  std::vector<TextRun> layout(const components::ComputedTextComponent& text,
                              const TextLayoutParams& params);

  FontVMetrics fontVMetrics(FontHandle font) const;
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const;
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const;
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const;
  PathSpline glyphOutline(FontHandle font, int glyphIndex, float scale) const;
  bool isBitmapOnly(FontHandle font) const;
  std::optional<TextBackend::BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                                      float scale) const;

  /// Measure the `ch` unit in ems for the given font-family cascade.
  std::optional<double> measureChUnitInEm(std::span<const RcString> fontFamilies);

private:
  FontManager& fontManager_;
  std::unique_ptr<TextBackend> backend_;
  size_t registeredFontFaceCount_ = 0;
};

}  // namespace donner::svg
