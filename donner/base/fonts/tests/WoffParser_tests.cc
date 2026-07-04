#include "donner/base/fonts/WoffParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/Runfiles.h"

namespace donner::fonts {

namespace {

std::vector<uint8_t> LoadFile(const std::string& location) {
  std::ifstream file(location, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Failed to open file: " << location;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

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

void WriteBE16(std::vector<uint8_t>* out, std::size_t offset, uint16_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void WriteBE32(std::vector<uint8_t>* out, std::size_t offset, uint32_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  (*out)[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

uint32_t Tag(std::string_view value) {
  EXPECT_EQ(value.size(), 4u);
  return (static_cast<uint32_t>(static_cast<uint8_t>(value[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(value[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(value[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(value[3]));
}

struct TableSpec {
  uint32_t tag = 0;
  std::vector<uint8_t> data;
  uint32_t origLengthOverride = 0;
  uint32_t compLengthOverride = 0;
  uint32_t offsetOverride = 0;
};

std::vector<uint8_t> MakeWoff(std::vector<TableSpec> tables) {
  std::vector<uint8_t> bytes;
  bytes.reserve(44 + tables.size() * 20);
  AppendBE32(&bytes, Tag("wOFF"));
  AppendBE32(&bytes, Tag("OTTO"));
  const std::size_t lengthOffset = bytes.size();
  AppendBE32(&bytes, 0);
  AppendBE16(&bytes, static_cast<uint16_t>(tables.size()));
  AppendBE16(&bytes, 0);
  AppendBE32(&bytes, 0);
  AppendBE16(&bytes, 1);
  AppendBE16(&bytes, 0);
  AppendBE32(&bytes, 0);
  AppendBE32(&bytes, 0);
  AppendBE32(&bytes, 0);
  AppendBE32(&bytes, 0);
  AppendBE32(&bytes, 0);

  const std::size_t directoryOffset = bytes.size();
  bytes.resize(bytes.size() + tables.size() * 20);
  std::size_t tableOffset = bytes.size();
  for (std::size_t i = 0; i < tables.size(); ++i) {
    const TableSpec& table = tables[i];
    const std::size_t recordOffset = directoryOffset + i * 20;
    const uint32_t offset =
        table.offsetOverride == 0 ? static_cast<uint32_t>(tableOffset) : table.offsetOverride;
    const uint32_t compLength = table.compLengthOverride == 0
                                    ? static_cast<uint32_t>(table.data.size())
                                    : table.compLengthOverride;
    const uint32_t origLength = table.origLengthOverride == 0
                                    ? static_cast<uint32_t>(table.data.size())
                                    : table.origLengthOverride;
    WriteBE32(&bytes, recordOffset + 0, table.tag);
    WriteBE32(&bytes, recordOffset + 4, offset);
    WriteBE32(&bytes, recordOffset + 8, compLength);
    WriteBE32(&bytes, recordOffset + 12, origLength);
    WriteBE32(&bytes, recordOffset + 16, 0);
    if (table.offsetOverride == 0) {
      bytes.insert(bytes.end(), table.data.begin(), table.data.end());
      tableOffset = bytes.size();
    }
  }

  WriteBE32(&bytes, lengthOffset, static_cast<uint32_t>(bytes.size()));
  return bytes;
}

struct NameRecordSpec {
  uint16_t platform = 3;
  uint16_t encoding = 1;
  uint16_t nameId = 1;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> Utf16Be(std::initializer_list<uint16_t> codepoints) {
  std::vector<uint8_t> bytes;
  for (uint16_t codepoint : codepoints) {
    AppendBE16(&bytes, codepoint);
  }
  return bytes;
}

std::vector<uint8_t> Ascii(std::string_view value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

std::vector<uint8_t> MakeNameTable(std::vector<NameRecordSpec> records) {
  std::vector<uint8_t> bytes;
  AppendBE16(&bytes, 0);
  AppendBE16(&bytes, static_cast<uint16_t>(records.size()));
  const uint16_t stringOffset = static_cast<uint16_t>(6 + records.size() * 12);
  AppendBE16(&bytes, stringOffset);

  std::size_t dataOffset = 0;
  for (const NameRecordSpec& record : records) {
    AppendBE16(&bytes, record.platform);
    AppendBE16(&bytes, record.encoding);
    AppendBE16(&bytes, 0);
    AppendBE16(&bytes, record.nameId);
    AppendBE16(&bytes, static_cast<uint16_t>(record.bytes.size()));
    AppendBE16(&bytes, static_cast<uint16_t>(dataOffset));
    dataOffset += record.bytes.size();
  }

  for (const NameRecordSpec& record : records) {
    bytes.insert(bytes.end(), record.bytes.begin(), record.bytes.end());
  }
  return bytes;
}

}  // namespace

TEST(WoffParser, Simple) {
  const std::string location =
      Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");

  std::vector<uint8_t> woffData = LoadFile(location);
  ASSERT_FALSE(woffData.empty()) << "WOFF file is empty: " << location;

  auto maybeWoffFont = WoffParser::Parse(woffData);
  ASSERT_THAT(maybeWoffFont, NoParseError());
}

/// Ensures WoffParser::Parse extracts the UTF-8 family name.
TEST(WoffParser, ExtractsFamilyName) {
  const std::string location =
      Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");

  std::vector<uint8_t> woffData = LoadFile(location);
  ASSERT_FALSE(woffData.empty()) << "WOFF file is empty: " << location;

  auto result = WoffParser::Parse(woffData);
  ASSERT_TRUE(result.hasResult()) << result.error().reason;
  const WoffFont& font = result.result();

  EXPECT_THAT(font.familyName, testing::Eq("WOFF Test CFF"));
}

TEST(WoffParser, RejectsMalformedHeadersAndDirectories) {
  const std::vector<uint8_t> tooShortBytes(43, 0);
  auto tooShort = WoffParser::Parse(tooShortBytes);
  ASSERT_TRUE(tooShort.hasError());
  EXPECT_THAT(tooShort.error().reason, testing::HasSubstr("too short"));

  std::vector<uint8_t> invalidSignature = MakeWoff({});
  invalidSignature[0] = 0;
  auto badSignature = WoffParser::Parse(invalidSignature);
  ASSERT_TRUE(badSignature.hasError());
  EXPECT_THAT(badSignature.error().reason, testing::HasSubstr("signature"));

  std::vector<uint8_t> lengthMismatch = MakeWoff({});
  WriteBE32(&lengthMismatch, 8, 99);
  auto badLength = WoffParser::Parse(lengthMismatch);
  ASSERT_TRUE(badLength.hasError());
  EXPECT_THAT(badLength.error().reason, testing::HasSubstr("length mismatch"));

  std::vector<uint8_t> truncatedDirectory = MakeWoff({});
  WriteBE16(&truncatedDirectory, 12, 1);
  auto badDirectory = WoffParser::Parse(truncatedDirectory);
  ASSERT_TRUE(badDirectory.hasError());
  EXPECT_THAT(badDirectory.error().reason, testing::HasSubstr("table directory"));
}

TEST(WoffParser, RejectsInvalidTableRangesAndSizes) {
  std::vector<uint8_t> outside = MakeWoff({
      TableSpec{
          .tag = Tag("test"),
          .data = {},
          .compLengthOverride = 8,
          .offsetOverride = 60,
      },
  });
  auto outsideResult = WoffParser::Parse(outside);
  ASSERT_TRUE(outsideResult.hasError());
  EXPECT_THAT(outsideResult.error().reason, testing::HasSubstr("outside of data"));

  std::vector<uint8_t> tooLarge = MakeWoff({
      TableSpec{
          .tag = Tag("test"),
          .data = {},
          .origLengthOverride = 30u * 1024u * 1024u + 1u,
      },
  });
  auto tooLargeResult = WoffParser::Parse(tooLarge);
  ASSERT_TRUE(tooLargeResult.hasError());
  EXPECT_THAT(tooLargeResult.error().reason, testing::HasSubstr("too large"));
}

TEST(WoffParser, ParsesSyntheticUncompressedTables) {
  const std::vector<uint8_t> woff = MakeWoff({
      TableSpec{
          .tag = Tag("cmap"),
          .data = {1, 2, 3, 4},
      },
  });

  auto result = WoffParser::Parse(woff);

  ASSERT_THAT(result, NoParseError());
  ASSERT_EQ(result.result().tables.size(), 1u);
  EXPECT_EQ(result.result().tables[0].tag, Tag("cmap"));
  EXPECT_THAT(result.result().tables[0].data, testing::ElementsAre(1, 2, 3, 4));
  EXPECT_EQ(result.result().familyName, std::nullopt);
}

TEST(WoffParser, ExtractsWindowsUnicodeFamilyName) {
  const std::vector<uint8_t> nameTable = MakeNameTable({
      NameRecordSpec{
          .platform = 3,
          .encoding = 10,
          .nameId = 1,
          .bytes = Utf16Be({'A', 0x00E9, 0x2603}),
      },
  });

  const std::vector<uint8_t> woff = MakeWoff({
      TableSpec{
          .tag = Tag("name"),
          .data = nameTable,
      },
  });
  auto result = WoffParser::Parse(woff);

  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(result.result().familyName, testing::Optional(std::string("A\303\251\342\230\203")));
}

TEST(WoffParser, FallsBackToTypographicMacFamilyName) {
  const std::vector<uint8_t> nameTable = MakeNameTable({
      NameRecordSpec{
          .platform = 1,
          .encoding = 0,
          .nameId = 16,
          .bytes = Ascii("Typographic Family"),
      },
  });

  const std::vector<uint8_t> woff = MakeWoff({
      TableSpec{
          .tag = Tag("name"),
          .data = nameTable,
      },
  });
  auto result = WoffParser::Parse(woff);

  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(result.result().familyName, testing::Optional(std::string("Typographic Family")));
}

TEST(WoffParser, IgnoresMalformedNameTables) {
  const std::vector<std::vector<uint8_t>> malformedNameTables = {
      {0, 0, 0, 1, 0},
      {0, 0, 0, 1, 0, 18},
      MakeNameTable({NameRecordSpec{
          .platform = 3,
          .encoding = 1,
          .nameId = 1,
          .bytes = {0, 'A', 0},
      }}),
      MakeNameTable({NameRecordSpec{
          .platform = 7,
          .encoding = 0,
          .nameId = 1,
          .bytes = Ascii("Unsupported"),
      }}),
  };

  for (const std::vector<uint8_t>& nameTable : malformedNameTables) {
    const std::vector<uint8_t> woff = MakeWoff({
        TableSpec{
            .tag = Tag("name"),
            .data = nameTable,
        },
    });
    auto result = WoffParser::Parse(woff);

    ASSERT_THAT(result, NoParseError());
    EXPECT_EQ(result.result().familyName, std::nullopt);
  }
}

}  // namespace donner::fonts
