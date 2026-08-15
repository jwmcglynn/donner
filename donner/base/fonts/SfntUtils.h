#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace donner::fonts {

/// Maximum number of entries accepted in an sfnt table directory.
inline constexpr size_t kMaximumSfntTables = 4096;

/// Maximum number of glyph frames reached while expanding a TrueType compound glyph.
inline constexpr size_t kMaximumCompoundGlyphDepth = 32;

/// Maximum component records stored or expanded by one accepted TrueType font.
inline constexpr size_t kMaximumCompoundComponentRecords = 65536;

/// Maximum simple-glyph points examined while validating one TrueType font.
inline constexpr size_t kMaximumSimpleGlyphPoints = 16 * 1024 * 1024;

/// Maximum vertices that stb_truetype may allocate while expanding one TrueType glyph.
inline constexpr size_t kMaximumExpandedGlyphVertices = 1024 * 1024;

/// Maximum decode, transform, and prefix-copy work allowed for one TrueType glyph outline.
inline constexpr size_t kMaximumGlyphOutlineWork = 16 * 1024 * 1024;

/// Read a 16-bit big-endian unsigned integer from @p p.
inline uint16_t ReadBe16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

/// Read a 32-bit big-endian unsigned integer from @p p.
inline uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// Convert a four-byte sfnt tag to its big-endian integer representation.
uint32_t SfntTag(std::string_view tag);

/**
 * Validated, allocation-bounded index for one sfnt font.
 *
 * Table records are sorted once, making lookup O(log T). TrueType `glyf` fonts also retain a
 * validated `loca` index and are accepted only when every compound dependency is acyclic, at most
 * @ref kMaximumCompoundGlyphDepth frames deep, and within the expanded vertex and outline-work
 * caps. The work model includes repeated and shared descendants, simple point decoding, component
 * transforms, and stb_truetype's repeated prefix copies.
 *
 * CFF/CFF2 tables receive bounded directory validation only. They must be consumed by an
 * exact-span parser such as FreeType; stb_truetype's CFF parser uses a synthetic 512 MiB span and
 * must not be initialized for these fonts.
 */
class SfntFont {
public:
  /// A table location validated against the original font byte span.
  struct TableRecord {
    uint32_t tag = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  SfntFont();
  ~SfntFont();
  SfntFont(SfntFont&&) noexcept;
  SfntFont& operator=(SfntFont&&) noexcept;

  SfntFont(const SfntFont&) = delete;
  SfntFont& operator=(const SfntFont&) = delete;

  /**
   * Validate and index @p data.
   *
   * @param data Complete sfnt byte stream.
   * @return A cached index on success, or std::nullopt for malformed or over-limit input.
   */
  static std::optional<SfntFont> Validate(std::span<const uint8_t> data);

  /**
   * Find a validated table in @p data.
   *
   * @p data must be the same immutable byte stream passed to @ref Validate.
   *
   * @param data Original sfnt byte stream.
   * @param tag Four-byte table tag.
   * @return A bounded table span, or std::nullopt when absent.
   */
  std::optional<std::span<const uint8_t>> findTable(std::span<const uint8_t> data,
                                                    std::string_view tag) const;

  /// Return true when the validated directory contains @p tag.
  bool hasTable(std::string_view tag) const;

  /// Number of validated directory entries.
  size_t numTables() const { return numTables_; }

  /// Number of glyphs represented by the retained `loca` index, or zero for non-TrueType fonts.
  size_t numGlyphs() const { return numGlyphs_; }

  /// Exact dynamic bytes retained by the sorted directory and `loca` index.
  size_t retainedBytes() const;

private:
  const TableRecord* findRecord(uint32_t tag) const;

  std::unique_ptr<TableRecord[]> tables_;
  size_t numTables_ = 0;
  std::unique_ptr<uint32_t[]> glyphOffsets_;
  size_t numGlyphs_ = 0;
};

/// Return true when @p data passes the bounded sfnt and outline validation policy.
bool ValidateSfnt(std::span<const uint8_t> data);

/**
 * Return a bounded sfnt table by its four-byte tag.
 *
 * This compatibility helper performs a fresh, explicitly bounded validation. Repeated callers
 * should retain an @ref SfntFont and call @ref SfntFont::findTable instead.
 */
std::optional<std::span<const uint8_t>> FindSfntTable(std::span<const uint8_t> data,
                                                      std::string_view tag);

}  // namespace donner::fonts
