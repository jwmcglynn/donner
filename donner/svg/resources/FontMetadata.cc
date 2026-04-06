#include "donner/svg/resources/FontMetadata.h"

namespace donner::svg {

namespace {

uint16_t readU16(std::span<const uint8_t> data, size_t offset) {
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

uint32_t readU32(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

std::string decodeNameRecord(std::span<const uint8_t> data, size_t stringStorageOffset,
                             size_t recordOffset) {
  const uint16_t length = readU16(data, recordOffset + 8);
  const uint16_t offset = readU16(data, recordOffset + 10);
  const size_t stringStart = stringStorageOffset + offset;
  if (stringStart + length > data.size()) {
    return {};
  }

  std::string result;
  for (size_t i = 0; i + 1 < length; i += 2) {
    const uint16_t ch =
        static_cast<uint16_t>((data[stringStart + i] << 8) | data[stringStart + i + 1]);
    result.push_back(ch < 128 ? static_cast<char>(ch) : '?');
  }

  return result;
}

std::optional<size_t> findTable(std::span<const uint8_t> data, const char* tag) {
  if (data.size() < 12) {
    return std::nullopt;
  }

  const uint16_t numTables = readU16(data, 4);
  for (uint16_t i = 0; i < numTables; ++i) {
    const size_t recordOffset = 12 + static_cast<size_t>(i) * 16;
    if (recordOffset + 16 > data.size()) {
      return std::nullopt;
    }

    if (data[recordOffset] == tag[0] && data[recordOffset + 1] == tag[1] &&
        data[recordOffset + 2] == tag[2] && data[recordOffset + 3] == tag[3]) {
      const size_t tableOffset = readU32(data, recordOffset + 8);
      if (tableOffset < data.size()) {
        return tableOffset;
      }
      return std::nullopt;
    }
  }

  return std::nullopt;
}

}  // namespace

std::optional<FontMetadata> ParseFontMetadata(std::span<const uint8_t> data) {
  if (data.size() < 12) {
    return std::nullopt;
  }

  FontMetadata metadata;

  const auto nameTableOffset = findTable(data, "name");
  if (!nameTableOffset.has_value() || *nameTableOffset + 6 > data.size()) {
    return std::nullopt;
  }

  const uint16_t nameCount = readU16(data, *nameTableOffset + 2);
  const size_t stringStorageOffset = *nameTableOffset + readU16(data, *nameTableOffset + 4);

  std::string familyNameId1;
  for (uint16_t i = 0; i < nameCount; ++i) {
    const size_t recordOffset = *nameTableOffset + 6 + static_cast<size_t>(i) * 12;
    if (recordOffset + 12 > data.size()) {
      break;
    }

    const uint16_t platformId = readU16(data, recordOffset);
    const uint16_t nameId = readU16(data, recordOffset + 6);
    if (platformId != 3) {
      continue;
    }

    if (nameId == 16) {
      metadata.familyName = decodeNameRecord(data, stringStorageOffset, recordOffset);
      if (!metadata.familyName.empty()) {
        break;
      }
    } else if (nameId == 1 && familyNameId1.empty()) {
      familyNameId1 = decodeNameRecord(data, stringStorageOffset, recordOffset);
    }
  }

  if (metadata.familyName.empty()) {
    metadata.familyName = std::move(familyNameId1);
  }
  if (metadata.familyName.empty()) {
    return std::nullopt;
  }

  const auto os2TableOffset = findTable(data, "OS/2");
  if (!os2TableOffset.has_value()) {
    return metadata;
  }

  if (*os2TableOffset + 8 <= data.size()) {
    const uint16_t widthClass = readU16(data, *os2TableOffset + 6);
    if (widthClass >= 1 && widthClass <= 9) {
      metadata.fontStretch = widthClass;
    }
  }

  if (*os2TableOffset + 6 <= data.size()) {
    metadata.fontWeight = readU16(data, *os2TableOffset + 4);
  }

  if (*os2TableOffset + 64 <= data.size()) {
    const uint16_t fsSelection = readU16(data, *os2TableOffset + 62);
    if (fsSelection & (1 << 9)) {
      metadata.fontStyle = 2;
    } else if (fsSelection & 1) {
      metadata.fontStyle = 1;
    }
  }

  return metadata;
}

}  // namespace donner::svg
