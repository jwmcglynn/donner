#pragma once
/// @file

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

/// Internal, frame-owned mapping from authored text entities to one rendered instance's styles.
/// Callers pass it synchronously; layout and draw results retain no reference to this map.
struct TextStyleContext {
  Entity rootEntity = entt::null;  ///< Instance root supplying viewport and root style.
  std::unordered_map<Entity, Entity> entities;  ///< Authored entity to instance style entity.

  /// Find an instance style without falling back to another instance.
  /// @param source Authored span entity. @return Matching style entity, or null when absent.
  Entity styleEntityFor(Entity source) const {
    const auto found = entities.find(source);
    return found == entities.end() ? entt::null : found->second;
  }
};

/// Build the bounded authored-to-occurrence style mapping for one shadow text root.
std::optional<TextStyleContext> BuildShadowTextStyleContext(
    Registry& registry, Entity root, const components::ComputedTextComponent& text,
    std::size_t* remainingWork = nullptr);

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
  /// Construct a text engine using the provided font manager and ECS registry.
  TextEngine(FontManager& fontManager, Registry& registry);

  /// Test-only constructor that injects a custom TextBackend.
  TextEngine(FontManager& fontManager, Registry& registry, std::unique_ptr<TextBackend> backend);

  ~TextEngine();

  TextEngine(const TextEngine&) = delete;
  TextEngine& operator=(const TextEngine&) = delete;

  /// Register a single \p face with the engine's font manager.
  void addFontFace(const css::FontFace& face);
  /// Register a batch of \p faces with the engine's font manager.
  void addFontFaces(std::span<const css::FontFace> faces);

  /// Prepare text dependencies for the text root containing \p handle.
  void prepareForElement(EntityHandle handle, ParseWarningSink& warningSink);

  /// Adopt pending face resolutions under serialized document access and invalidate only the
  /// text roots whose cached runs used a changed face. Call before the renderer's dirty snapshot.
  std::vector<Entity> refreshFontResources();
  /// Whether changed or pending font resources require another text preparation pass.
  bool needsFontResourceRefresh() const;

  /// Lay out all spans, returning positioned glyph runs.
  std::vector<TextRun> layout(const components::ComputedTextComponent& text,
                              const TextLayoutParams& params);

  /// Number of full text layouts performed by this engine, for CPU-work regression tests.
  [[nodiscard]] std::size_t layoutCallCountForTesting() const { return layoutCallCount_; }

  /// Most face and codepoint coverage answers that font fallback retains for one text element.
  static constexpr std::size_t kMaximumFallbackCoverageEntries = 16384;

  /// Coverage answers that font fallback retained during the most recent layout, for
  /// memory-bound regression tests.
  [[nodiscard]] std::size_t fallbackCoverageEntriesForTesting() const {
    return fallbackCoverageEntries_;
  }

  /// Span bounding-box entries examined while building or answering object-box queries.
  [[nodiscard]] std::size_t objectBoundingBoxSpanVisitsForTesting() const {
    return objectBoundingBoxSpanVisits_;
  }

  /// Resolve a style owner's face and adjusted used size for declaring-element font metrics.
  ResolvedTextFont resolveUsedFont(EntityHandle styleOwner, const Box2d& viewBox,
                                   const FontMetrics& fontMetrics) const;

  /// Return vertical font metrics for \p font.
  FontVMetrics fontVMetrics(FontHandle font) const;
  /// Return the scale factor mapping design units to \p pixelHeight pixels for \p font.
  float scaleForPixelHeight(FontHandle font, float pixelHeight) const;
  /// Return the scale factor mapping one em to \p pixelHeight pixels for \p font.
  float scaleForEmToPixels(FontHandle font, float pixelHeight) const;
  /// Return underline position/thickness for \p font, if available.
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const;
  /// Return strikeout position/thickness for \p font, if available.
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle font) const;
  /// Return subscript/superscript offset metrics for \p font, if available.
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const;
  /// Return the vector outline for \p glyphIndex in \p font at the given \p scale.
  Path glyphOutline(FontHandle font, int glyphIndex, float scale) const;
  /// Extract an outline without changing the shared face's synthesis configuration.
  /// @param font Face whose glyph is extracted.
  /// @param glyphIndex Glyph index in the face.
  /// @param scale Font-unit to pixel scale.
  /// @param allowSyntheticOblique Per-glyph permission to synthesize an oblique outline.
  /// @return The glyph outline, preserving native style and variation axes.
  Path glyphOutline(FontHandle font, int glyphIndex, float scale, bool allowSyntheticOblique) const;
  /// Return true if \p font only contains bitmap glyphs (e.g. color emoji).
  bool isBitmapOnly(FontHandle font) const;
  /// Return a rasterized bitmap glyph for \p glyphIndex in \p font at the given \p scale.
  std::optional<TextBackend::BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                                      float scale) const;

  /// Query bitmap cost and placement without decoding pixels.
  /// @param font Font to query.
  /// @param glyphIndex Glyph within the font.
  /// @param scale Requested scale.
  std::optional<TextBackend::BitmapGlyphMetrics> bitmapGlyphMetrics(FontHandle font, int glyphIndex,
                                                                    float scale) const;

  /// Decode an admitted bitmap only after revalidating its metadata and RGBA byte cap.
  /// @param font Font to decode.
  /// @param glyphIndex Glyph within the font.
  /// @param scale Requested scale.
  /// @param admitted Metadata whose cost the caller has reserved.
  /// @param maximumRgbaBytes Maximum converted RGBA storage allowed by that reservation.
  std::optional<TextBackend::BitmapGlyph> bitmapGlyph(
      FontHandle font, int glyphIndex, float scale, const TextBackend::BitmapGlyphMetrics& admitted,
      std::size_t maximumRgbaBytes) const;

  /// Measure the `ch` unit in ems for the given font-family cascade.
  std::optional<double> measureChUnitInEm(std::span<const RcString> fontFamilies,
                                          Entity geometryOwner = entt::null);

  /// Resolve per-span layout-affecting style state on a computed text tree.
  void resolvePerSpanLayoutStyles(EntityHandle textRootHandle,
                                  components::ComputedTextComponent& text,
                                  const TextStyleContext* styleContext = nullptr) const;

  /// Ensure cached text geometry exists for the text root containing \p handle.
  const components::ComputedTextGeometryComponent& ensureComputedTextGeometryComponent(
      EntityHandle handle) const;

  /// A placed glyph outline paired with the source element that produced it.
  struct GlyphOutline {
    Path path;                         ///< Glyph outline in text-element local coordinates.
    Entity sourceEntity = entt::null;  ///< Span source entity (text/tspan) that owns this glyph.
  };

  /// Visibility-independent reference geometry in text-root local coordinates.
  struct ReferenceGeometry {
    std::optional<Box2d> objectBounds;   ///< Full glyph-cell union, including hidden glyphs.
    std::vector<GlyphOutline> outlines;  ///< Optional source-tagged outlines for stroke bounds.
    std::vector<FontFaceDependency> fontDependencies;  ///< Faces consulted by this query.
  };

  /// Internal rendering query for horizontal reference geometry without a painted-ink cache.
  /// @internal
  /// Vertical glyph-cell geometry is unsupported and explicitly refused.
  /// @param handle Text content subtree to include.
  /// @param includeOutlines Whether stroke-reference outlines are required.
  /// @param remainingWork Shared input, decode and geometry work allowance; zeroed on refusal.
  /// @return Empty optional on refusal, otherwise the admitted geometry (which can be empty).
  std::optional<ReferenceGeometry> referenceGeometry(
      EntityHandle handle, bool includeOutlines, std::size_t& remainingWork,
      const TextStyleContext* styleContext = nullptr) const;

  /// Return glyph outlines for the text subtree rooted at \p handle.
  std::vector<Path> computedGlyphPaths(EntityHandle handle) const;

  /// Return glyph outlines paired with their source entities for the subtree rooted at \p handle.
  /// The paint that colored each glyph lives on its source entity's computed style; callers that
  /// must preserve per-span paint (e.g. text-to-outlines) resolve style from `sourceEntity`.
  std::vector<GlyphOutline> computedGlyphOutlines(EntityHandle handle) const;

  /// Return the ink bounds for the text subtree rooted at \p handle.
  Box2d computedInkBounds(EntityHandle handle) const;

  /// Return the object bounding box for the text subtree rooted at \p handle.
  Box2d computedObjectBoundingBox(EntityHandle handle) const;

  /**
   * @brief The root \ref xml_text element that \p handle takes part in, if any.
   *
   * Every geometry query here resolves through the text root and requires one to exist. Element
   * types that carry text components without always being text content, notably \ref xml_a, reach
   * those queries from outside any text element, so callers test this first.
   *
   * @param handle The element to resolve.
   * @return The root text entity, or `entt::null` when \p handle is not inside a text element.
   */
  Entity textRootEntity(EntityHandle handle) const;

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
  Box2d getExtentOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the rotation of a character for the text subtree rooted at \p handle.
  double getRotationOfChar(EntityHandle handle, std::size_t charnum) const;

  /// Return the character index at the given point for the text subtree rooted at \p handle.
  long getCharNumAtPosition(EntityHandle handle, const Vector2d& point) const;

private:
  std::vector<TextRun> layoutWithResourceStatus(const components::ComputedTextComponent& text,
                                                const TextLayoutParams& params,
                                                bool& resourceRejected);
  FontManager& fontManager_;
  Registry& registry_;
  std::unique_ptr<TextBackend> backend_;
  size_t registeredFontFaceCount_ = 0;
  uint64_t observedFontResourceRevision_ = 0;
  std::size_t layoutCallCount_ = 0;
  std::size_t fallbackCoverageEntries_ = 0;
  mutable std::size_t objectBoundingBoxSpanVisits_ = 0;
};

}  // namespace donner::svg
