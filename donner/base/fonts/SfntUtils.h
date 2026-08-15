#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace donner::fonts {

/// Read a 32-bit big-endian unsigned integer from p.
inline uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// Return true when data is a structurally bounded sfnt (TTF/OTF) container.
inline bool ValidateSfnt(std::span<const uint8_t> data) {
  if (data.size() < 12) {
    return false;
  }

  const uint32_t magic = ReadBe32(data.data());
  if (magic != 0x00010000 && magic != 0x4F54544F && magic != 0x74727565 && magic != 0x74797031) {
    return false;
  }

  const size_t numTables = (static_cast<size_t>(data[4]) << 8) | data[5];
  if (numTables > (data.size() - 12) / 16) {
    return false;
  }

  for (size_t i = 0; i < numTables; ++i) {
    const size_t recordOffset = 12 + i * 16;
    const size_t tableOffset = ReadBe32(data.data() + recordOffset + 8);
    const size_t tableLength = ReadBe32(data.data() + recordOffset + 12);
    if (tableOffset > data.size() || tableLength > data.size() - tableOffset) {
      return false;
    }
  }
  return true;
}

/// Return a bounded sfnt table by its four-byte tag.
inline std::optional<std::span<const uint8_t>> FindSfntTable(std::span<const uint8_t> data,
                                                             std::string_view tag) {
  if (tag.size() != 4 || !ValidateSfnt(data)) {
    return std::nullopt;
  }

  const size_t numTables = (static_cast<size_t>(data[4]) << 8) | data[5];
  for (size_t i = 0; i < numTables; ++i) {
    const size_t recordOffset = 12 + i * 16;
    if (data[recordOffset] == static_cast<uint8_t>(tag[0]) &&
        data[recordOffset + 1] == static_cast<uint8_t>(tag[1]) &&
        data[recordOffset + 2] == static_cast<uint8_t>(tag[2]) &&
        data[recordOffset + 3] == static_cast<uint8_t>(tag[3])) {
      const size_t tableOffset = ReadBe32(data.data() + recordOffset + 8);
      const size_t tableLength = ReadBe32(data.data() + recordOffset + 12);
      return data.subspan(tableOffset, tableLength);
    }
  }
  return std::nullopt;
}

}  // namespace donner::fonts
