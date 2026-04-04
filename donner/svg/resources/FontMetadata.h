#pragma once
/// @file

#include <optional>
#include <span>
#include <string>

namespace donner::svg {

/**
 * Metadata extracted from a raw OpenType/TrueType font file.
 */
struct FontMetadata {
  std::string familyName;  ///< CSS font-family name from the name table.
  int fontWeight = 400;    ///< CSS font-weight (100-900).
  int fontStyle = 0;       ///< CSS font-style (0=normal, 1=italic, 2=oblique).
  int fontStretch = 5;     ///< CSS font-stretch (1-9, 5=normal).
};

/**
 * Parse CSS-relevant metadata from a raw OpenType/TrueType font file.
 *
 * @param data Raw font bytes.
 * @return Metadata if the required tables are present and readable.
 */
std::optional<FontMetadata> ParseFontMetadata(std::span<const uint8_t> data);

}  // namespace donner::svg
