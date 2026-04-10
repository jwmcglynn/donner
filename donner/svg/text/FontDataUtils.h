#pragma once
/// @file

#include <cstdint>
#include <span>

namespace donner::svg {

/// Read a 32-bit big-endian unsigned integer from \p p.
inline uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// Check if raw font data has a valid 'head' table and read unitsPerEm. Returns 0 if missing.
inline uint16_t ReadUnitsPerEm(std::span<const uint8_t> data) {
  if (data.size() < 12) {
    return 0;
  }

  const int numTables = (data[4] << 8) | data[5];
  for (int i = 0; i < numTables; ++i) {
    const size_t off = 12 + static_cast<size_t>(i) * 16;
    if (off + 16 > data.size()) {
      break;
    }

    if (data[off] == 'h' && data[off + 1] == 'e' && data[off + 2] == 'a' &&
        data[off + 3] == 'd') {
      const uint32_t tableOff = ReadBe32(data.data() + off + 8);
      if (tableOff + 20 <= data.size()) {
        return static_cast<uint16_t>((data[tableOff + 18] << 8) | data[tableOff + 19]);
      }
    }
  }

  return 0;
}

/// Returns true if the raw font has a scalable outline table.
inline bool HasOutlineTables(std::span<const uint8_t> data) {
  if (data.size() < 12) {
    return false;
  }

  const int numTables = (data[4] << 8) | data[5];
  for (int i = 0; i < numTables; ++i) {
    const size_t off = 12 + static_cast<size_t>(i) * 16;
    if (off + 16 > data.size()) {
      break;
    }

    const bool isGlyf =
        data[off] == 'g' && data[off + 1] == 'l' && data[off + 2] == 'y' && data[off + 3] == 'f';
    const bool isCff =
        data[off] == 'C' && data[off + 1] == 'F' && data[off + 2] == 'F' && data[off + 3] == ' ';
    const bool isCff2 =
        data[off] == 'C' && data[off + 1] == 'F' && data[off + 2] == 'F' && data[off + 3] == '2';

    if (isGlyf || isCff || isCff2) {
      return true;
    }
  }

  return false;
}

}  // namespace donner::svg
