#include "donner/base/fonts/Woff2Parser.h"

#include <fstream>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::fonts {

namespace {

/// Read a file into a byte vector.
std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  EXPECT_TRUE(file.good()) << "Failed to open: " << path;
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

}  // namespace

TEST(Woff2ParserTest, DecompressValid) {
  auto woff2Data = readFile("donner/base/fonts/testdata/valid-001.woff2");
  ASSERT_FALSE(woff2Data.empty());

  auto result = Woff2Parser::Decompress(woff2Data);
  ASSERT_FALSE(result.hasError()) << result.error().reason;

  const auto& sfnt = result.result();
  EXPECT_GT(sfnt.size(), 0u);

  // The decompressed output should be valid for stb_truetype.
  stbtt_fontinfo info{};
  int initResult = stbtt_InitFont(&info, sfnt.data(), 0);
  EXPECT_NE(initResult, 0) << "stbtt_InitFont should succeed on decompressed WOFF2";

  // Verify we can look up a basic Latin glyph.
  int glyphIndex = stbtt_FindGlyphIndex(&info, 'A');
  EXPECT_GT(glyphIndex, 0) << "Should find glyph for 'A'";
}

TEST(Woff2ParserTest, DataTooShort) {
  std::vector<uint8_t> data = {0x77, 0x4F, 0x46};  // 3 bytes, too short
  auto result = Woff2Parser::Decompress(data);
  EXPECT_TRUE(result.hasError());
}

TEST(Woff2ParserTest, EmptyData) {
  auto result = Woff2Parser::Decompress({});
  EXPECT_TRUE(result.hasError());
}

TEST(Woff2ParserTest, InvalidMagic) {
  // Valid size but wrong magic bytes.
  std::vector<uint8_t> data(100, 0);
  data[0] = 0x00;
  data[1] = 0x01;
  data[2] = 0x00;
  data[3] = 0x00;
  auto result = Woff2Parser::Decompress(data);
  EXPECT_TRUE(result.hasError());
}

TEST(Woff2ParserTest, TruncatedWoff2) {
  // Start with a valid WOFF2 file, then truncate it.
  auto woff2Data = readFile("donner/base/fonts/testdata/valid-001.woff2");
  ASSERT_GT(woff2Data.size(), 100u);

  // Truncate to just the header area.
  woff2Data.resize(48);
  auto result = Woff2Parser::Decompress(woff2Data);
  EXPECT_TRUE(result.hasError());
}

}  // namespace donner::fonts
