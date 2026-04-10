#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/css/FontFace.h"

namespace donner::svg {

/**
 * Opaque handle to a loaded font, used to reference fonts in the FontManager.
 */
class FontHandle {
public:
  /// Default-constructed handle is invalid.
  FontHandle() = default;

  /// Returns true if this handle is valid (refers to a loaded font).
  explicit operator bool() const { return entity_ != entt::null; }

  /// Equality comparison.
  bool operator==(const FontHandle& other) const { return entity_ == other.entity_; }
  /// Inequality comparison.
  bool operator!=(const FontHandle& other) const { return entity_ != other.entity_; }

  /// Get the raw entity identifier (for internal use and hash maps).
  Entity entity() const { return entity_; }

private:
  friend class FontManager;
  explicit FontHandle(Entity entity) : entity_(entity) {}
  Entity entity_ = entt::null;
};

}  // namespace donner::svg

/// std::hash specialization so \ref donner::svg::FontHandle can be used as a key in
/// unordered associative containers.
template <>
struct std::hash<donner::svg::FontHandle> {
  /// Returns a hash value derived from the underlying entity identifier.
  size_t operator()(const donner::svg::FontHandle& h) const noexcept {
    return std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(h.entity()));
  }
};

namespace donner::svg {

/**
 * Manages font loading, caching, and lookup for text rendering.
 *
 * FontManager is the shared font infrastructure used by both the Skia and TinySkia backends.
 * It handles:
 * - Loading raw TTF/OTF font data.
 * - Loading WOFF 1.0 fonts via the existing WoffParser, reconstructing the sfnt byte stream,
 *   then storing the reconstructed sfnt bytes.
 * - Resolving `@font-face` source cascades.
 * - Caching resolved family/style lookups to avoid repeated face scans.
 * - Falling back to the embedded Public Sans font when no match is found.
 * - Storing backend caches on the same font entity as the loaded bytes.
 *
 * FontManager uses entt entities to store font data, with one entity per registered `@font-face`
 * rule or directly-loaded font. Text backends can cache parsed backend objects directly on the same
 * entity.
 */
class FontManager {
public:
  /// Construct a FontManager tied to the provided ECS \p registry.
  explicit FontManager(Registry& registry);
  /// Destructor.
  ~FontManager();

  // Non-copyable, non-movable.
  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;
  FontManager(FontManager&&) = delete;
  FontManager& operator=(FontManager&&) = delete;

  /**
   * Register a `@font-face` declaration. Sources are resolved lazily on first `findFont()` call
   * for the corresponding family name.
   *
   * @param face The parsed `@font-face` rule.
   */
  void addFontFace(const css::FontFace& face);

  /**
   * Register a mapping from a CSS generic family name (serif, sans-serif, monospace, cursive,
   * fantasy) to a real font family name registered via `addFontFace()`.
   *
   * This allows `findFont("sans-serif")` to resolve to the specified family.
   *
   * @param genericName The CSS generic family name (case-insensitive).
   * @param realFamily The real family name to resolve to.
   */
  void setGenericFamilyMapping(std::string_view genericName, std::string_view realFamily);

  /**
   * Find or load a font matching the given family name.
   *
   * Resolution order:
   * 1. If the family is a CSS generic name with a registered mapping, resolve to the real name.
   * 2. Return an already-loaded entity if the best matching face has already been resolved.
   * 3. Walk registered `@font-face` rules whose `font-family` matches, trying each source.
   * 4. Fall back to the embedded Public Sans font.
   *
   * @param family Font family name to look up.
   * @return A valid FontHandle, or an invalid handle if even the fallback fails.
   */
  FontHandle findFont(std::string_view family);

  /**
   * Find or load a font matching the given family name and weight.
   *
   * @param family Font family name to look up.
   * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
   * @return A valid FontHandle, or falls back to findFont(family) if no weight match.
   */
  FontHandle findFont(std::string_view family, int weight);

  /**
   * Find or load a font matching the given family name, weight, style, and stretch.
   *
   * @param family Font family name to look up.
   * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
   * @param style CSS font-style value (0=normal, 1=italic, 2=oblique).
   * @param stretch CSS font-stretch value (1-9, 5=normal, matching FontStretch enum).
   * @return A valid FontHandle, or falls back to findFont(family, weight) if no match.
   */
  FontHandle findFont(std::string_view family, int weight, int style, int stretch);

  /**
   * Load a font from raw TTF/OTF/WOFF data.
   *
   * The data is copied internally. The font is not associated with any family name; callers
   * should use `findFont()` for name-based lookup.
   *
   * @param data Raw font file bytes (TTF, OTF, or WOFF 1.0).
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadFontData(std::span<const uint8_t> data);

  /**
   * Get the raw font data bytes for a handle.
   *
   * This is useful for backends (like Skia) that need to create their own font objects from
   * the raw data.
   *
   * @param handle A valid FontHandle.
   * @return Span of the raw font data, or empty span if the handle is invalid.
   */
  std::span<const uint8_t> fontData(FontHandle handle) const;

  /**
   * Get the number of registered `@font-face` rules.
   */
  size_t numFaces() const;

  /**
   * Get the family name of a registered `@font-face` rule by index.
   *
   * @param index Index into the registered faces (0 to numFaces()-1).
   * @return The family name, or empty string_view if index is out of range.
   */
  std::string_view faceFamilyName(size_t index) const;

  /**
   * Get the handle for the embedded fallback font (Public Sans).
   */
  FontHandle fallbackFont();

private:
  struct FontFaceComponent;
  struct LoadedFontComponent;

  /**
   * Internal: load raw TTF/OTF data (not WOFF) from an owned buffer.
   *
   * @param entity Target entity.
   * @param data Owned font data buffer. Must remain valid for the lifetime of the FontManager.
   * @return True on success.
   */
  bool setRawFontData(Entity entity, std::vector<uint8_t> data);
  bool setRawFontData(Entity entity, std::shared_ptr<const std::vector<uint8_t>> sharedData);
  bool loadFontDataSharedIntoEntity(Entity entity,
                                    const std::shared_ptr<const std::vector<uint8_t>>& data);

  /**
   * Internal: load a WOFF 1.0 font by parsing and reconstructing the sfnt byte stream.
   *
   * @param entity Target entity.
   * @param data Raw WOFF data.
   * @return True on success.
   */
  bool loadWoff1(Entity entity, std::span<const uint8_t> data);

#ifdef DONNER_TEXT_WOFF2_ENABLED
  /**
   * Internal: load a WOFF 2.0 font by decompressing via Brotli and table transforms.
   *
   * Only available when built with the `text_full` feature flag.
   *
   * @param entity Target entity.
   * @param data Raw WOFF2 data.
   * @return True on success.
   */
  bool loadWoff2(Entity entity, std::span<const uint8_t> data);
#endif

  /**
   * Internal: load font bytes into an existing entity.
   *
   * @param entity Target entity.
   * @param data Raw font file bytes.
   * @return True on success.
   */
  bool loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data);

  /// Returns true if \p handle refers to a live font entity in the registry.
  bool isValidHandle(FontHandle handle) const;

  /// Internal EnTT storage for font faces, loaded font bytes, and backend caches.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Cache: family/style query key → resolved font handle.
  std::unordered_map<std::string, FontHandle> cache_;

  /// Mapping from CSS generic family names to real family names.
  std::unordered_map<std::string, std::string> genericFamilyMap_;

  /// Handle for the embedded Public Sans fallback, lazily loaded.
  FontHandle fallbackHandle_;
};

}  // namespace donner::svg
