#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "donner/css/FontFace.h"

struct stbtt_fontinfo;

namespace donner::svg {

/**
 * Opaque handle to a loaded font, used to reference fonts in the FontManager.
 */
class FontHandle {
public:
  /// Default-constructed handle is invalid.
  FontHandle() = default;

  /// Returns true if this handle is valid (refers to a loaded font).
  explicit operator bool() const { return index_ >= 0; }

  /// Equality comparison.
  bool operator==(const FontHandle& other) const { return index_ == other.index_; }
  bool operator!=(const FontHandle& other) const { return index_ != other.index_; }

  /// Get the raw index (for internal use and hash maps).
  int index() const { return index_; }

private:
  friend class FontManager;
  explicit FontHandle(int index) : index_(index) {}
  int index_ = -1;
};

}  // namespace donner::svg

template <>
struct std::hash<donner::svg::FontHandle> {
  size_t operator()(const donner::svg::FontHandle& h) const noexcept {
    return std::hash<int>{}(h.index());
  }
};

namespace donner::svg {

/**
 * Manages font loading, caching, and lookup for text rendering.
 *
 * FontManager is the shared font infrastructure used by both the Skia and TinySkia backends.
 * It handles:
 * - Loading raw TTF/OTF font data via stb_truetype.
 * - Loading WOFF 1.0 fonts via the existing WoffParser, reconstructing the sfnt byte stream,
 *   then initializing via stb_truetype.
 * - Resolving `@font-face` source cascades.
 * - Falling back to the embedded Public Sans font when no match is found.
 * - Caching loaded fonts by family name for reuse.
 *
 * The font data is owned by FontManager and remains valid for the lifetime of the manager,
 * which is required since stb_truetype holds a pointer into the font data buffer.
 */
class FontManager {
public:
  FontManager();
  ~FontManager();

  // Non-copyable, non-movable (stbtt_fontinfo holds pointers into owned data).
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
   * Find or load a font matching the given family name.
   *
   * Resolution order:
   * 1. Return a cached font if one exists for this family.
   * 2. Walk registered `@font-face` rules whose `font-family` matches, trying each source.
   * 3. Fall back to the embedded Public Sans font.
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
   * Access the stb_truetype font info for a handle.
   *
   * @param handle A valid FontHandle obtained from findFont() or loadFontData().
   * @return Pointer to the stbtt_fontinfo, or nullptr if the handle is invalid or bitmap-only.
   */
  const stbtt_fontinfo* fontInfo(FontHandle handle) const;

  /**
   * Returns true if the font is bitmap-only (e.g., CBDT color emoji).
   * These fonts have no glyf outlines and require FreeType for rendering.
   */
  bool isBitmapOnly(FontHandle handle) const;

  /**
   * Get the scale factor to produce a font whose EM-square maps to the given pixel height.
   *
   * @param handle A valid FontHandle.
   * @param pixelHeight Desired font height in pixels.
   * @return Scale factor, or 0 if the handle is invalid.
   */
  float scaleForPixelHeight(FontHandle handle, float pixelHeight) const;

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
   * Get the number of registered @font-face rules.
   */
  size_t numFaces() const { return faces_.size(); }

  /**
   * Get the family name of a registered @font-face rule by index.
   *
   * @param index Index into the registered faces (0 to numFaces()-1).
   * @return The family name, or empty string_view if index is out of range.
   */
  std::string_view faceFamilyName(size_t index) const {
    if (index < faces_.size()) {
      return faces_[index].familyName;
    }
    return {};
  }

  /**
   * Get the handle for the embedded fallback font (Public Sans).
   */
  FontHandle fallbackFont();

private:
  struct LoadedFont;

  /**
   * Internal: load raw TTF/OTF data (not WOFF) from an owned buffer.
   *
   * @param data Owned font data buffer. Must remain valid for the lifetime of the FontManager.
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadRawFont(std::vector<uint8_t> data);
  FontHandle loadRawFont(std::shared_ptr<const std::vector<uint8_t>> sharedData);
  FontHandle loadFontDataShared(const std::shared_ptr<const std::vector<uint8_t>>& data);

  /**
   * Internal: load a WOFF 1.0 font by parsing and reconstructing the sfnt byte stream.
   *
   * @param data Raw WOFF data.
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadWoff1(std::span<const uint8_t> data);

#ifdef DONNER_TEXT_WOFF2_ENABLED
  /**
   * Internal: load a WOFF 2.0 font by decompressing via Brotli and table transforms.
   *
   * Only available when built with the `text_woff2` feature flag.
   *
   * @param data Raw WOFF2 data.
   * @return A valid FontHandle on success, or an invalid handle on failure.
   */
  FontHandle loadWoff2(std::span<const uint8_t> data);
#endif

  /// Registered @font-face rules.
  std::vector<css::FontFace> faces_;

  /// Cache: family name → FontHandle.
  std::unordered_map<std::string, FontHandle> cache_;

  /// All loaded fonts. Indices correspond to FontHandle::index_.
  std::vector<std::unique_ptr<LoadedFont>> fonts_;

  /// Handle for the embedded Public Sans fallback, lazily loaded.
  FontHandle fallbackHandle_;
};

}  // namespace donner::svg
