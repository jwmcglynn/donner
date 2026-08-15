#include "donner/base/fonts/Woff2Parser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <vector>

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

std::array<uint8_t, 48> minimalWoff2Header() {
  std::array<uint8_t, 48> data{};
  data[0] = 0x77;
  data[1] = 0x4F;
  data[2] = 0x46;
  data[3] = 0x32;
  data[11] = 48;
  data[13] = 1;
  return data;
}

void writeBigEndianU32(std::span<uint8_t> data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}

size_t writeBase128(std::span<uint8_t> data, size_t offset, uint32_t value) {
  size_t encodedSize = 1;
  for (uint32_t remaining = value; remaining >= 128; remaining >>= 7) {
    ++encodedSize;
  }

  for (size_t i = 0; i < encodedSize; ++i) {
    const size_t shift = 7 * (encodedSize - i - 1);
    data[offset + i] = static_cast<uint8_t>((value >> shift) & 0x7F);
    if (i + 1 != encodedSize) {
      data[offset + i] |= 0x80;
    }
  }
  return offset + encodedSize;
}

std::vector<uint8_t> woff2WithIntermediateSize(uint32_t intermediateSize) {
  constexpr size_t kInputSize = 64u * 1024u;
  std::vector<uint8_t> data(kInputSize, 0);
  data[0] = 0x77;
  data[1] = 0x4F;
  data[2] = 0x46;
  data[3] = 0x32;
  writeBigEndianU32(data, 4, 0x00010000u);
  writeBigEndianU32(data, 8, static_cast<uint32_t>(data.size()));
  data[13] = 1;
  writeBigEndianU32(data, 16, 64u * 1024u * 1024u);

  size_t directoryEnd = 48;
  data[directoryEnd++] = 0;
  directoryEnd = writeBase128(data, directoryEnd, intermediateSize);
  writeBigEndianU32(data, 20, static_cast<uint32_t>(data.size() - directoryEnd));
  return data;
}

std::vector<uint8_t> woff2WithTransformedGlyfSize(uint32_t transformedSize) {
  constexpr size_t kInputSize = 64u * 1024u;
  std::vector<uint8_t> data(kInputSize, 0);
  data[0] = 0x77;
  data[1] = 0x4F;
  data[2] = 0x46;
  data[3] = 0x32;
  writeBigEndianU32(data, 4, 0x00010000u);
  writeBigEndianU32(data, 8, static_cast<uint32_t>(data.size()));
  data[13] = 1;
  writeBigEndianU32(data, 16, 64u * 1024u * 1024u);

  size_t directoryEnd = 48;
  data[directoryEnd++] = 10;  // Known-tag index for transformed glyf, transform version zero.
  directoryEnd = writeBase128(data, directoryEnd, transformedSize);
  directoryEnd = writeBase128(data, directoryEnd, transformedSize);
  writeBigEndianU32(data, 20, static_cast<uint32_t>(data.size() - directoryEnd));
  return data;
}

std::vector<uint8_t> malformedOneTableWoff2WithDeclaredOutput(uint32_t outputSize) {
  std::vector<uint8_t> data(50, 0);
  data[0] = 0x77;
  data[1] = 0x4F;
  data[2] = 0x46;
  data[3] = 0x32;
  writeBigEndianU32(data, 4, 0x00010000u);
  writeBigEndianU32(data, 8, static_cast<uint32_t>(data.size()));
  data[13] = 1;
  writeBigEndianU32(data, 16, outputSize);
  writeBigEndianU32(data, 28, 0x08080808u);
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

TEST(Woff2ParserTest, RejectsInputLargerThanLimit) {
  Woff2Parser::Options options;
  options.maximumInputSize = 3;
  auto result = Woff2Parser::Decompress(std::vector<uint8_t>(4), options);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error().reason, testing::HasSubstr("input exceeds limit"));
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

TEST(Woff2ParserTest, RejectsInvalidSignatureBeforeDecompression) {
  constexpr std::array<uint8_t, 21> data = {
      0x00, 0xFF, 0xFF, 0xFF, 0xD0, 0xFF, 0xFF, 0x5D, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF,
  };

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: invalid signature");
}

TEST(Woff2ParserTest, RejectsIncompleteHeaderBeforeDecompression) {
  constexpr std::array<uint8_t, 4> data = {0x77, 0x4F, 0x46, 0x32};

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: incomplete header");
}

TEST(Woff2ParserTest, RejectsMismatchedInputLengthBeforeDecompression) {
  auto data = minimalWoff2Header();
  data[11] = 47;

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: declared input length does not match data");
}

TEST(Woff2ParserTest, RejectsMissingTablesBeforeDecompression) {
  auto data = minimalWoff2Header();
  data[13] = 0;

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: header declares no tables");
}

TEST(Woff2ParserTest, RejectsNonzeroReservedFieldBeforeDecompression) {
  auto data = minimalWoff2Header();
  data[15] = 1;

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: reserved header field must be zero");
}

TEST(Woff2ParserTest, RejectsMalformedStreamWithoutAllocatingDeclaredOutput) {
  auto result =
      Woff2Parser::Decompress(malformedOneTableWoff2WithDeclaredOutput(32u * 1024u * 1024u));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: decompression failed");
}

TEST(Woff2ParserTest, RejectsLinuxTimeoutSeedAtTableCountPreflight) {
  constexpr std::array<uint8_t, 48> data = {
      0x77, 0x4F, 0x46, 0x32, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x30,
      0x3A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x25, 0x08, 0x08, 0x08, 0x08, 0x08, 0xFA, 0xF7, 0xF7, 0xF7, 0xF7,
      0xF7, 0xF7, 0xF7, 0x08, 0x08, 0x08, 0x08, 0x08, 0x77, 0x0A, 0x4F, 0x32,
  };

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: table count exceeds limit");
}

TEST(Woff2ParserTest, CorpusRegressionSeedsRetainExpectedPreflightFailures) {
  struct CorpusExpectation {
    const char* filename;
    const char* reason;
  };
  constexpr std::array kExpectations = {
      CorpusExpectation{"regression-declared-size-allocation.woff2", "WOFF2: decompression failed"},
      CorpusExpectation{"regression-intermediate-allocation.woff2",
                        "WOFF2: intermediate decompressed size exceeds limit"},
      CorpusExpectation{"regression-invalid-signature.woff2", "WOFF2: invalid signature"},
      CorpusExpectation{"regression-linux-timeout.woff2", "WOFF2: table count exceeds limit"},
      CorpusExpectation{"regression-transformed-glyf-allocation.woff2",
                        "WOFF2: transformed glyf size exceeds limit"},
  };

  for (const auto& expectation : kExpectations) {
    SCOPED_TRACE(expectation.filename);
    const auto data =
        readFile(std::string("donner/base/fonts/tests/woff2_corpus/") + expectation.filename);
    ASSERT_FALSE(data.empty());
    const auto result = Woff2Parser::Decompress(data);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().reason, expectation.reason);
  }
}

TEST(Woff2ParserTest, RejectsOversizedIntermediateBufferBeforeDecoderEntry) {
  auto result = Woff2Parser::Decompress(woff2WithIntermediateSize(16u * 1024u * 1024u + 1u));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: intermediate decompressed size exceeds limit");
}

TEST(Woff2ParserTest, AllowsIntermediateBufferAtLimitIntoDecoder) {
  auto result = Woff2Parser::Decompress(woff2WithIntermediateSize(16u * 1024u * 1024u));
  ASSERT_TRUE(result.hasError());
  EXPECT_NE(result.error().reason, "WOFF2: intermediate decompressed size exceeds limit");
}

TEST(Woff2ParserTest, AcceptsLargeUntransformedTableAboveGlyfScratchLimit) {
  // Generated from the bundled Roboto-Regular.ttf by appending a 5 MiB opaque gvar table and
  // encoding with the pinned Google WOFF2 encoder. The large table is ordinary Brotli output, not
  // the transformed glyf stream whose decoder scratch work needs the tighter limit.
  auto data = readFile("donner/base/fonts/testdata/large-untransformed-table.woff2");
  ASSERT_FALSE(data.empty());

  auto result = Woff2Parser::Decompress(data);
  ASSERT_FALSE(result.hasError()) << result.error().reason;
  EXPECT_GE(result.result().size(), 5u * 1024u * 1024u);
}

TEST(Woff2ParserTest, RejectsOversizedTransformedGlyfBeforeDecoderScratchAllocation) {
  auto result = Woff2Parser::Decompress(woff2WithTransformedGlyfSize(4u * 1024u * 1024u + 1u));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: transformed glyf size exceeds limit");
}

TEST(Woff2ParserTest, AllowsTransformedGlyfAtScratchLimitIntoDecoder) {
  auto result = Woff2Parser::Decompress(woff2WithTransformedGlyfSize(4u * 1024u * 1024u));
  ASSERT_TRUE(result.hasError());
  EXPECT_NE(result.error().reason, "WOFF2: transformed glyf size exceeds limit");
}

TEST(Woff2ParserTest, RejectsExcessiveTableCountBeforeDirectoryAllocation) {
  auto data = minimalWoff2Header();
  data[12] = 0x10;
  data[13] = 0x01;
  data[19] = 1;

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: table count exceeds limit");
}

TEST(Woff2ParserTest, RejectsExcessiveCollectionFontCountBeforeAllocation) {
  const auto header = minimalWoff2Header();
  std::vector<uint8_t> data(header.begin(), header.end());
  data.resize(56, 0);
  writeBigEndianU32(data, 4, 0x74746366u);
  writeBigEndianU32(data, 8, static_cast<uint32_t>(data.size()));
  data[19] = 1;
  data[48] = 0;
  data[49] = 0;
  writeBigEndianU32(data, 50, 0x00010000u);
  data[54] = 255;
  data[55] = 4;

  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().reason, "WOFF2: collection font count exceeds limit");
}

TEST(Woff2ParserTest, RejectsOversizedDeclaredSize) {
  // Regression: a complete WOFF2 header whose totalSfntSize field (bytes 16-19)
  // declares ~4 GiB. ComputeWOFF2FinalSize returns that value verbatim, so without
  // the size guard this attempts a multi-gigabyte allocation before decompression.
  auto data = minimalWoff2Header();
  data[16] = 0xFF;
  data[17] = 0xFF;
  data[18] = 0xFF;
  data[19] = 0xFF;
  auto result = Woff2Parser::Decompress(data);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error().reason, testing::HasSubstr("exceeds limit"));
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
