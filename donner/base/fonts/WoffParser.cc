#include "donner/base/fonts/WoffParser.h"

#include <cstring>

#include "donner/base/encoding/Decompress.h"

namespace donner::fonts {
namespace {

uint32_t ReadBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t ReadBE16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

/**
 * Converts a UTF‑16BE string to UTF‑8.
 *
 * Used for 'name' table records on Windows/Unicode platforms.
 *
 * @param utf16be The UTF‑16BE encoded string as a byte span.
 * @return        The UTF‑8 encoded string.
 */
std::string Utf16BeToUtf8(std::span<const uint8_t> utf16be) {
  std::string out;
  out.reserve(utf16be.size());  // worst‑case 1 UTF‑8 byte per 16‑bit code unit
  for (size_t i = 0; i + 1 < utf16be.size(); i += 2) {
    uint16_t code = ReadBE16(utf16be.data() + i);
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }
  return out;
}

/**
 * Attempts to extract the font family name from a NAME table.
 *
 * @param nameTable Raw 'name' table bytes.
 * @return          UTF‑8 family name, or \c std::nullopt if not found/decodable.
 */
std::optional<std::string> ParseNameTable(std::span<const uint8_t> nameTable) {
  // The table header is 6 bytes: format, count, stringOffset.
  if (nameTable.size() < 6) {
    return std::nullopt;
  }
  const uint16_t count = ReadBE16(nameTable.data() + 2);
  const uint16_t stringOffset = ReadBE16(nameTable.data() + 4);

  // Each NameRecord is 12 bytes.
  const size_t recordsSize = static_cast<size_t>(count) * 12;
  if (6 + recordsSize > nameTable.size()) {
    return std::nullopt;
  }

  auto pickRecord = [&](uint16_t wantedNameId) -> const uint8_t* {
    const uint8_t* rec = nameTable.data() + 6;
    for (uint16_t i = 0; i < count; ++i, rec += 12) {
      const uint16_t platform = ReadBE16(rec + 0);
      const uint16_t encoding = ReadBE16(rec + 2);
      /*const uint16_t language =*/(void)ReadBE16(rec + 4);
      const uint16_t nameId = ReadBE16(rec + 6);
      if (nameId != wantedNameId) {
        continue;
      }
      // Prefer Windows Unicode (platform 3, encoding 1 or 10).
      if (platform == 3 && (encoding == 1 || encoding == 10)) {
        return rec;
      }
    }
    // Fallback: return the first record with the requested name‑id.
    rec = nameTable.data() + 6;
    for (uint16_t i = 0; i < count; ++i, rec += 12) {
      if (ReadBE16(rec + 6) == wantedNameId) {
        return rec;
      }
    }
    return nullptr;
  };

  const uint8_t* record = pickRecord(/*Font Family =*/1);
  if (!record) {
    // Try Typographic Family (name‑id 16) as a last resort.
    record = pickRecord(/*Typographic Family =*/16);
    if (!record) {
      return std::nullopt;
    }
  }

  const uint16_t length = ReadBE16(record + 8);
  const uint16_t offset = ReadBE16(record + 10);

  if (stringOffset + offset + length > nameTable.size()) {
    return std::nullopt;
  }
  std::span<const uint8_t> raw(nameTable.data() + stringOffset + offset, length);
  const uint16_t platform = ReadBE16(record);
  const uint16_t encoding = ReadBE16(record + 2);

  // Windows‑Unicode -> UTF‑16BE
  if (platform == 3 && (encoding == 1 || encoding == 10)) {
    if (raw.size() % 2 != 0) {
      return std::nullopt;  // broken UTF‑16 length
    }
    return Utf16BeToUtf8(raw);
  }

  // Macintosh Roman -> 8‑bit, nearly ASCII; accept as‑is.
  if (platform == 1) {
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
  }

  // Unsupported encoding.
  return std::nullopt;
}

struct TableRecord {
  uint32_t tag;
  uint32_t offset;
  uint32_t compLength;
  uint32_t origLength;
  uint32_t checksum;
};

}  // namespace

ParseResult<WoffFont> WoffParser::Parse(std::span<const uint8_t> bytes) {
  if (bytes.size() < 44) {
    ParseError err;
    err.reason = "WOFF data too short";
    return err;
  }

  const uint8_t* p = bytes.data();
  const uint32_t signature = ReadBE32(p);
  p += 4;
  if (signature != 0x774F4646) {  // 'wOFF'
    ParseError err;
    err.reason = "Invalid WOFF signature";
    return err;
  }

  WoffFont font;
  font.flavor = ReadBE32(p);
  p += 4;
  const uint32_t length = ReadBE32(p);
  p += 4;
  const uint16_t numTables = ReadBE16(p);
  p += 2;
  p += 2;  // reserved
  const uint32_t totalSfntSize = ReadBE32(p);
  p += 4;
  (void)totalSfntSize;
  p += 2;  // majorVersion
  p += 2;  // minorVersion
  const uint32_t metaOffset = ReadBE32(p);
  p += 4;
  const uint32_t metaLength = ReadBE32(p);
  p += 4;
  const uint32_t metaOrigLength = ReadBE32(p);
  p += 4;
  const uint32_t privOffset = ReadBE32(p);
  p += 4;
  const uint32_t privLength = ReadBE32(p);
  p += 4;
  (void)metaOffset;
  (void)metaLength;
  (void)metaOrigLength;
  (void)privOffset;
  (void)privLength;

  if (length != bytes.size()) {
    ParseError err;
    err.reason = "WOFF length mismatch";
    return err;
  }

  if (44 + numTables * 20 > bytes.size()) {
    ParseError err;
    err.reason = "Truncated WOFF table directory";
    return err;
  }

  std::vector<TableRecord> records;
  records.reserve(numTables);
  const uint8_t* dir = bytes.data() + 44;
  for (uint16_t i = 0; i < numTables; ++i) {
    TableRecord rec{};
    rec.tag = ReadBE32(dir);
    dir += 4;
    rec.offset = ReadBE32(dir);
    dir += 4;
    rec.compLength = ReadBE32(dir);
    dir += 4;
    rec.origLength = ReadBE32(dir);
    dir += 4;
    rec.checksum = ReadBE32(dir);
    dir += 4;
    if (rec.offset + rec.compLength > bytes.size()) {
      ParseError err;
      err.reason = "Table outside of data";
      return err;
    }
    records.push_back(rec);
  }

  font.tables.reserve(numTables);
  for (const auto& rec : records) {
    std::vector<uint8_t> table;

    const uint8_t* src = bytes.data() + rec.offset;
    if (rec.compLength == rec.origLength) {
      table.resize(rec.origLength);
      std::memcpy(table.data(), src, rec.origLength);
    } else {
      auto maybeDecompressedData = Decompress::Zlib(
          std::string_view(reinterpret_cast<const char*>(src), rec.compLength), rec.origLength);
      if (maybeDecompressedData.hasError()) {
        // TODO(jwm): The error should be annotated with the table tag.
        return std::move(maybeDecompressedData.error());
      }

      table = std::move(maybeDecompressedData.result());
    }

    font.tables.push_back({rec.tag, std::move(table)});
  }

  // Extract family name from the 'name' table if available.
  constexpr uint32_t kNameTag = 0x6E616D65;  // 'name'
  for (const auto& t : font.tables) {
    if (t.tag == kNameTag) {
      font.familyName = ParseNameTable(t.data);
      break;
    }
  }

  return font;
}

}  // namespace donner::fonts
