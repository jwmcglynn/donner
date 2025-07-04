#pragma once
/**
 * @file WoffFont.h
 *
 * Value types for representing a Web Open Font Format (WOFF) font and its underlying sfnt tables
 * once they have been decompressed into memory.
 *
 * These plain structs are intentionally lightweight so low‑level parsing code can use them without
 * introducing additional dynamic allocation.
 */

#include <cstdint>
#include <vector>

namespace donner::fonts {

/**
 * Single sfnt table extracted from a WOFF container.
 *
 * The table is identified by its four‑character `tag` and stores its uncompressed binary `data`
 * payload exactly as it appears in the original font file.
 */
struct WoffTable {
  uint32_t tag = 0;           ///< Table four-character tag.
  std::vector<uint8_t> data;  ///< Uncompressed table data.
};

/**
 * In‑memory representation of a complete WOFF font.
 *
 * `flavor` stores the sfnt flavor (for example 0x00010000 for TrueType or the four‑character code
 * 'OTTO' for OpenType‑CFF). The `tables` vector contains all decompressed tables in the order they
 * were encountered in the source file.
 */
struct WoffFont {
  uint32_t flavor = 0;            ///< SFNT flavor, e.g. 0x00010000 or 'OTTO'.
  std::vector<WoffTable> tables;  ///< Parsed tables.
};

}  // namespace donner::fonts
