#include "donner/svg/resources/FontMetadata.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
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

struct NameRecordSpec {
  uint16_t platformId = 3;
  uint16_t nameId = 1;
  std::vector<uint8_t> text;
};

std::vector<uint8_t> Utf16Be(std::initializer_list<uint16_t> chars) {
  std::vector<uint8_t> data;
  for (uint16_t ch : chars) {
    AppendBE16(&data, ch);
  }
  return data;
}

std::vector<uint8_t> Utf16Be(std::string_view text) {
  std::vector<uint8_t> data;
  for (char ch : text) {
    AppendBE16(&data, static_cast<uint8_t>(ch));
  }
  return data;
}

std::vector<uint8_t> MakeNameTable(std::vector<NameRecordSpec> records) {
  std::vector<uint8_t> data;
  AppendBE16(&data, 0);
  AppendBE16(&data, static_cast<uint16_t>(records.size()));
  const uint16_t stringStorageOffset = static_cast<uint16_t>(6 + records.size() * 12);
  AppendBE16(&data, stringStorageOffset);

  size_t textOffset = 0;
  for (const NameRecordSpec& record : records) {
    AppendBE16(&data, record.platformId);
    AppendBE16(&data, 1);
    AppendBE16(&data, 0);
    AppendBE16(&data, record.nameId);
    AppendBE16(&data, static_cast<uint16_t>(record.text.size()));
    AppendBE16(&data, static_cast<uint16_t>(textOffset));
    textOffset += record.text.size();
  }

  for (const NameRecordSpec& record : records) {
    data.insert(data.end(), record.text.begin(), record.text.end());
  }
  return data;
}

std::vector<uint8_t> MakeOs2Table(uint16_t weight, uint16_t widthClass, uint16_t fsSelection) {
  std::vector<uint8_t> data(64, 0);
  WriteBE16(&data, 4, weight);
  WriteBE16(&data, 6, widthClass);
  WriteBE16(&data, 62, fsSelection);
  return data;
}

TEST(FontMetadata, RejectsMissingAndMalformedNameTables) {
  EXPECT_EQ(ParseFontMetadata({}), std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "head", .offset = 28, .data = {}}})),
            std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "nxxx", .offset = 28, .data = {}}})),
            std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "naxx", .offset = 28, .data = {}}})),
            std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "namx", .offset = 28, .data = {}}})),
            std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "name", .offset = 128, .data = {}}})),
            std::nullopt);
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{.tag = "name", .offset = 28, .data = {0, 0}}})),
            std::nullopt);

  std::vector<uint8_t> truncatedDirectory = MakeSfnt({
      TableSpec{.tag = "name", .offset = 28, .data = MakeNameTable({})},
  });
  truncatedDirectory.resize(20);
  EXPECT_EQ(ParseFontMetadata(truncatedDirectory), std::nullopt);

  std::vector<uint8_t> truncatedRecord;
  AppendBE16(&truncatedRecord, 0);
  AppendBE16(&truncatedRecord, 1);
  AppendBE16(&truncatedRecord, 18);
  EXPECT_EQ(ParseFontMetadata(
                MakeSfnt({TableSpec{.tag = "name", .offset = 28, .data = truncatedRecord}})),
            std::nullopt);
}

TEST(FontMetadata, IgnoresUnsupportedAndOutOfRangeNameRecords) {
  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{
                .tag = "name",
                .offset = 28,
                .data = MakeNameTable({NameRecordSpec{
                    .platformId = 1,
                    .nameId = 1,
                    .text = Utf16Be("Ignored"),
                }}),
            }})),
            std::nullopt);

  EXPECT_EQ(ParseFontMetadata(MakeSfnt({TableSpec{
                .tag = "name",
                .offset = 28,
                .data = MakeNameTable({NameRecordSpec{
                    .platformId = 3,
                    .nameId = 2,
                    .text = Utf16Be("Subfamily"),
                }}),
            }})),
            std::nullopt);

  std::vector<uint8_t> outOfRange = MakeNameTable({NameRecordSpec{
      .platformId = 3,
      .nameId = 1,
      .text = Utf16Be("Broken"),
  }});
  WriteBE16(&outOfRange, 6 + 10, 200);
  EXPECT_EQ(
      ParseFontMetadata(MakeSfnt({TableSpec{.tag = "name", .offset = 28, .data = outOfRange}})),
      std::nullopt);
}

TEST(FontMetadata, PrefersTypographicFamilyAndExtractsOs2Metadata) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 44,
          .data = MakeNameTable({
              NameRecordSpec{.platformId = 3, .nameId = 1, .text = Utf16Be("Fallback")},
              NameRecordSpec{.platformId = 3, .nameId = 16, .text = Utf16Be("Preferred")},
          }),
      },
      TableSpec{
          .tag = "OS/2",
          .offset = 128,
          .data = MakeOs2Table(700, 7, 1u << 9),
      },
  });

  const std::optional<FontMetadata> metadata = ParseFontMetadata(data);

  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "Preferred");
  EXPECT_EQ(metadata->fontWeight, 700);
  EXPECT_EQ(metadata->fontStretch, 7);
  EXPECT_EQ(metadata->fontStyle, 2);
}

TEST(FontMetadata, FallsBackToFamilyNameAndItalicStyle) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 44,
          .data = MakeNameTable({
              NameRecordSpec{.platformId = 1, .nameId = 1, .text = Utf16Be("Ignored")},
              NameRecordSpec{.platformId = 3, .nameId = 1, .text = Utf16Be({'A', 0x00E9})},
          }),
      },
      TableSpec{
          .tag = "OS/2",
          .offset = 128,
          .data = MakeOs2Table(300, 10, 1),
      },
  });

  const std::optional<FontMetadata> metadata = ParseFontMetadata(data);

  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "A?");
  EXPECT_EQ(metadata->fontWeight, 300);
  EXPECT_EQ(metadata->fontStretch, 5);
  EXPECT_EQ(metadata->fontStyle, 1);
}

TEST(FontMetadata, KeepsFirstFamilyNameRecord) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 28,
          .data = MakeNameTable({
              NameRecordSpec{.platformId = 3, .nameId = 1, .text = Utf16Be("First")},
              NameRecordSpec{.platformId = 3, .nameId = 1, .text = Utf16Be("Second")},
          }),
      },
  });

  const std::optional<FontMetadata> metadata = ParseFontMetadata(data);

  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "First");
}

TEST(FontMetadata, IgnoresPartialOs2TagsAndInvalidWidthLowerBound) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 92,
          .data = MakeNameTable({NameRecordSpec{
              .platformId = 3,
              .nameId = 1,
              .text = Utf16Be("Partial Os2"),
          }}),
      },
      TableSpec{
          .tag = "Oxxx",
          .offset = 160,
          .data = MakeOs2Table(900, 9, 1u << 9),
      },
      TableSpec{
          .tag = "OSxx",
          .offset = 224,
          .data = MakeOs2Table(800, 8, 1),
      },
      TableSpec{
          .tag = "OS/x",
          .offset = 288,
          .data = MakeOs2Table(700, 7, 1),
      },
      TableSpec{
          .tag = "OS/2",
          .offset = 352,
          .data = MakeOs2Table(600, 0, 0),
      },
  });

  const std::optional<FontMetadata> metadata = ParseFontMetadata(data);

  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "Partial Os2");
  EXPECT_EQ(metadata->fontWeight, 600);
  EXPECT_EQ(metadata->fontStretch, 5);
  EXPECT_EQ(metadata->fontStyle, 0);
}

TEST(FontMetadata, ReturnsFamilyMetadataWhenOs2IsMissingOrShort) {
  const std::vector<uint8_t> withoutOs2 = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 28,
          .data = MakeNameTable({NameRecordSpec{
              .platformId = 3,
              .nameId = 1,
              .text = Utf16Be("No Os2"),
          }}),
      },
  });
  std::optional<FontMetadata> metadata = ParseFontMetadata(withoutOs2);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "No Os2");
  EXPECT_EQ(metadata->fontWeight, 400);
  EXPECT_EQ(metadata->fontStretch, 5);
  EXPECT_EQ(metadata->fontStyle, 0);

  const std::vector<uint8_t> shortOs2 = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 44,
          .data = MakeNameTable({NameRecordSpec{
              .platformId = 3,
              .nameId = 1,
              .text = Utf16Be("Short Os2"),
          }}),
      },
      TableSpec{
          .tag = "OS/2",
          .offset = 128,
          .data =
              [] {
                std::vector<uint8_t> data(6, 0);
                WriteBE16(&data, 4, 500);
                return data;
              }(),
      },
  });
  metadata = ParseFontMetadata(shortOs2);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "Short Os2");
  EXPECT_EQ(metadata->fontWeight, 500);
  EXPECT_EQ(metadata->fontStretch, 5);
  EXPECT_EQ(metadata->fontStyle, 0);

  const std::vector<uint8_t> tooShortForWeight = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 44,
          .data = MakeNameTable({NameRecordSpec{
              .platformId = 3,
              .nameId = 1,
              .text = Utf16Be("Tiny Os2"),
          }}),
      },
      TableSpec{
          .tag = "OS/2",
          .offset = 128,
          .data = std::vector<uint8_t>(4, 0),
      },
  });
  metadata = ParseFontMetadata(tooShortForWeight);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "Tiny Os2");
  EXPECT_EQ(metadata->fontWeight, 400);
  EXPECT_EQ(metadata->fontStretch, 5);
  EXPECT_EQ(metadata->fontStyle, 0);
}

TEST(FontMetadata, FallsBackFromEmptyTypographicFamily) {
  const std::vector<uint8_t> data = MakeSfnt({
      TableSpec{
          .tag = "name",
          .offset = 28,
          .data = MakeNameTable({
              NameRecordSpec{.platformId = 3, .nameId = 16, .text = {}},
              NameRecordSpec{.platformId = 3, .nameId = 1, .text = Utf16Be("Family")},
          }),
      },
  });

  const std::optional<FontMetadata> metadata = ParseFontMetadata(data);

  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->familyName, "Family");
}

}  // namespace
}  // namespace donner::svg
