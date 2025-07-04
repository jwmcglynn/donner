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

  return font;
}

}  // namespace donner::fonts
