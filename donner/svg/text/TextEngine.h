#pragma once
/// @file

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/ParseError.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextPathComponent.h"
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
 *   Registry registry;
 *   auto& fontManager = registry.ctx().emplace<FontManager>(registry);
 *   auto& engine = registry.ctx().emplace<TextEngine>(fontManager, registry);
 *   std::vector<TextRun> runs = engine.layout(text, params);
 * @endcode
 */
class TextEngine {
public:
  TextEngine(FontManager& fontManager, Registry& registry);

  /// Test-only constructor that injects a custom TextBackend.
  TextEngine(FontManager& fontManager, Registry& registry,
             std::unique_ptr<TextBackend> backend);

  ~TextEngine();

  TextEngine(const TextEngine&) = delete;
  TextEngine& operator=(const TextEngine&) = delete;

  void addFontFace(const css::FontFace& face);
  void addFontFaces(std::span<const css::FontFace> faces);

  /// Prepare text dependencies for the text root containing \p handle.
  void prepareForElement(EntityHandle handle, std::vector<ParseError>* outWarnings = nullptr);

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

  /// Resolve per-span layout-affecting style state on a computed text tree.
  void resolvePerSpanLayoutStyles(EntityHandle textRootHandle,
                                  components::ComputedTextComponent& text) const;

  /// Ensure cached text geometry exists for the text root containing \p handle.
  const components::ComputedTextPathComponent& ensureComputedTextPathComponent(
      EntityHandle handle) const;

  /// Return glyph outlines for the text subtree rooted at \p handle.
  std::vector<PathSpline> computedGlyphPaths(EntityHandle handle) const;

  /// Return the ink bounds for the text subtree rooted at \p handle.
  Boxd computedInkBounds(EntityHandle handle) const;

  /// Return the object bounding box for the text subtree rooted at \p handle.
  Boxd computedObjectBoundingBox(EntityHandle handle) const;

  /// Return the number of addressable characters for the text subtree rooted at \p handle.
  long getNumberOfChars(EntityHandle handle) const;

  /// Return the computed text length for the text subtree rooted at \p handle.
  double getComputedTextLength(EntityHandle handle) const;

  /// Return the substring length for the text subtree rooted at \p handle.
  double getSubStringLength(EntityHandle handle, std::size_t charnum, std::size_t nchars) const;

  /// Return the start position of a character for the text subtree rooted at \p handle.
  Vector2d getStartPositionOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the end position of a character for the text subtree rooted at \p handle.
  Vector2d getEndPositionOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the extent of a character for the text subtree rooted at \p handle.
  Boxd getExtentOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the rotation of a character for the text subtree rooted at \p handle.
  double getRotationOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the character index at the given point for the text subtree rooted at \p handle.
  long getCharNumAtPosition(EntityHandle handle, const Vector2d& point) const;

private:
  FontManager& fontManager_;
  Registry& registry_;
  std::unique_ptr<TextBackend> backend_;
  size_t registeredFontFaceCount_ = 0;
};

}  // namespace donner::svg
