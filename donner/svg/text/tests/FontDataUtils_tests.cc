#include "donner/svg/text/FontDataUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace donner::svg {
namespace {

void AppendBE16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendBE32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(value & 0xFF));
}

void WriteBE16(std::vector<uint8_t>* out, size_t offset, uint16_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void WriteBE32(std::vector<uint8_t>* out, size_t offset, uint32_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  (*out)[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

struct TableSpec {
  std::string_view tag;
  uint32_t offset;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> MakeSfnt(std::vector<TableSpec> tables) {
  std::vector<uint8_t> data;
  AppendBE32(&data, 0x00010000);
  AppendBE16(&data, static_cast<uint16_t>(tables.size()));
  AppendBE16(&data, 0);
  AppendBE16(&data, 0);
  AppendBE16(&data, 0);

  const size_t directoryOffset = data.size();
  data.resize(data.size() + tables.size() * 16);

  for (size_t i = 0; i < tables.size(); ++i) {
    const TableSpec& table = tables[i];
    const size_t recordOffset = directoryOffset + i * 16;
    EXPECT_EQ(table.tag.size(), 4u);
    data[recordOffset + 0] = static_cast<uint8_t>(table.tag[0]);
    data[recordOffset + 1] = static_cast<uint8_t>(table.tag[1]);
    data[recordOffset + 2] = static_cast<uint8_t>(table.tag[2]);
    data[recordOffset + 3] = static_cast<uint8_t>(table.tag[3]);
    WriteBE32(&data, recordOffset + 4, 0);
    WriteBE32(&data, recordOffset + 8, table.offset);
    WriteBE32(&data, recordOffset + 12, static_cast<uint32_t>(table.data.size()));

    if (data.size() < table.offset + table.data.size()) {
      data.resize(table.offset + table.data.size());
    }
    std::copy(table.data.begin(), table.data.end(), data.begin() + table.offset);
  }
  return data;
}

std::vector<uint8_t> HeadTable(uint16_t unitsPerEm) {
  std::vector<uint8_t> table(20, 0);
  WriteBE16(&table, 18, unitsPerEm);
  return table;
}

TEST(FontDataUtils, ReadUnitsPerEmRejectsShortOrMissingHeadData) {
  EXPECT_EQ(ReadUnitsPerEm({}), 0);
  EXPECT_EQ(ReadUnitsPerEm(MakeSfnt({TableSpec{.tag = "name", .offset = 28, .data = {}}})), 0);
  EXPECT_EQ(ReadUnitsPerEm(MakeSfnt({TableSpec{.tag = "hxxx", .offset = 28, .data = {}}})), 0);
  EXPECT_EQ(ReadUnitsPerEm(MakeSfnt({TableSpec{.tag = "hexx", .offset = 28, .data = {}}})), 0);
  EXPECT_EQ(ReadUnitsPerEm(MakeSfnt({TableSpec{.tag = "heax", .offset = 28, .data = {}}})), 0);

  std::vector<uint8_t> truncatedDirectory = MakeSfnt({
      TableSpec{.tag = "name", .offset = 28, .data = {}},
  });
  truncatedDirectory.resize(20);
  EXPECT_EQ(ReadUnitsPerEm(truncatedDirectory), 0);
}

TEST(FontDataUtils, ReadUnitsPerEmExtractsHeadTableWhenRangeIsValid) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{.tag = "head", .offset = 28, .data = HeadTable(2048)},
  });

  EXPECT_EQ(ReadUnitsPerEm(data), 2048);
}

TEST(FontDataUtils, ReadUnitsPerEmRejectsOutOfRangeHeadTable) {
  std::vector<uint8_t> data = MakeSfnt({
      TableSpec{.tag = "head", .offset = 128, .data = {}},
  });
  data.resize(44);

  EXPECT_EQ(ReadUnitsPerEm(data), 0);
}

TEST(FontDataUtils, HasOutlineTablesDetectsSupportedOutlineTags) {
  EXPECT_TRUE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "glyf", .offset = 28, .data = {}}})));
  EXPECT_TRUE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "CFF ", .offset = 28, .data = {}}})));
  EXPECT_TRUE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "CFF2", .offset = 28, .data = {}}})));
}

TEST(FontDataUtils, HasOutlineTablesRejectsShortTruncatedAndNonOutlineFonts) {
  EXPECT_FALSE(HasOutlineTables({}));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "name", .offset = 28, .data = {}}})));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "gxxx", .offset = 28, .data = {}}})));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "glxx", .offset = 28, .data = {}}})));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "glyx", .offset = 28, .data = {}}})));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "Cxxx", .offset = 28, .data = {}}})));
  EXPECT_FALSE(HasOutlineTables(MakeSfnt({TableSpec{.tag = "CFxx", .offset = 28, .data = {}}})));

  std::vector<uint8_t> truncatedDirectory = MakeSfnt({
      TableSpec{.tag = "glyf", .offset = 28, .data = {}},
  });
  truncatedDirectory.resize(20);
  EXPECT_FALSE(HasOutlineTables(truncatedDirectory));
}

}  // namespace
}  // namespace donner::svg
