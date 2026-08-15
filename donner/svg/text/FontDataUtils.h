#pragma once
/// @file

#include <cstdint>
#include <span>

#include "donner/base/fonts/SfntUtils.h"

namespace donner::svg {

/// Read a 32-bit big-endian unsigned integer from \p p.
using fonts::FindSfntTable;
using fonts::ReadBe32;
using fonts::ValidateSfnt;

/// Check if raw font data has a valid 'head' table and read unitsPerEm. Returns 0 if missing.
inline uint16_t ReadUnitsPerEm(std::span<const uint8_t> data) {
  const auto head = FindSfntTable(data, "head");
  if (head && head->size() >= 20) {
    return static_cast<uint16_t>(((*head)[18] << 8) | (*head)[19]);
  }
  return 0;
}

/// Returns true if the raw font has a scalable outline table.
inline bool HasOutlineTables(std::span<const uint8_t> data) {
  return FindSfntTable(data, "glyf").has_value() || FindSfntTable(data, "CFF ").has_value() ||
         FindSfntTable(data, "CFF2").has_value();
}

}  // namespace donner::svg
