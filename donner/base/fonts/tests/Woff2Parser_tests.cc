#include "donner/base/fonts/Woff2Parser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
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

// Build a complete header and an exact-length directory so truncation reaches our preflight
// rather than being hidden by trailing padding or by the WOFF2 decoder's allocation path.
std::vector<uint8_t> Woff2PreflightInput(std::span<const uint8_t> directory,
                                         uint32_t flavor = 0x00010000u) {
  const auto header = minimalWoff2Header();
  std::vector<uint8_t> data(header.begin(), header.end());
  data.insert(data.end(), directory.begin(), directory.end());
  writeBigEndianU32(data, 4, flavor);
  writeBigEndianU32(data, 8, static_cast<uint32_t>(data.size()));
  writeBigEndianU32(data, 16, 1024u);
  return data;
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

TEST(Woff2ParserTest, RejectsMalformedTableDirectoriesBeforeDecoderEntry) {
  const std::vector<std::pair<std::string_view, std::vector<uint8_t>>> cases = {
      {"missing flags", {}},
      {"truncated explicit tag", {0x3F, 0x67, 0x6C, 0x79}},
      {"missing length", {0}},
      {"forbidden base128 leading zero", {0, 0x80}},
      {"base128 exceeds five bytes", {0, 0x81, 0x80, 0x80, 0x80, 0x80}},
      {"transformed glyf missing length", {10, 1}},
      {"transformed loca has a nonzero length", {11, 1, 1}},
      {"transformed non-glyf missing length", {0x40, 1}},
  };
  for (const auto& [scenario, directory] : cases) {
    SCOPED_TRACE(scenario);
    const auto result = Woff2Parser::Decompress(Woff2PreflightInput(directory));
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().reason, "WOFF2: invalid table directory");
  }
}

TEST(Woff2ParserTest, CatalogRejectsWrongFlavorAndCffTablesBeforeDecoderEntry) {
  Woff2Parser::Options options;
  options.requireTrueTypeOutlines = true;
  struct Case {
    std::string_view scenario;
    std::vector<uint8_t> directory;
    uint32_t flavor;
    std::string_view reason;
  };
  const std::array cases = {
      Case{"wrong standalone flavor",
           {0, 1},
           0x4F54544Fu,
           "WOFF2: catalog requires standalone TrueType outlines"},
      Case{"known CFF tag",
           {13, 1},
           0x00010000u,
           "WOFF2: catalog requires TrueType outlines without CFF tables"},
      Case{"explicit CFF2 tag",
           {0x3F, 0x43, 0x46, 0x46, 0x32, 1},
           0x00010000u,
           "WOFF2: catalog requires TrueType outlines without CFF tables"},
  };
  for (const Case& testCase : cases) {
    SCOPED_TRACE(testCase.scenario);
    const auto result =
        Woff2Parser::Decompress(Woff2PreflightInput(testCase.directory, testCase.flavor), options);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().reason, testCase.reason);
  }
}

TEST(Woff2ParserTest, RejectsMalformedCollectionDirectoriesBeforeDecoderEntry) {
  constexpr uint32_t kCollectionFlavor = 0x74746366u;  // ttcf
  struct Case {
    std::string_view scenario;
    std::vector<uint8_t> directory;
    std::string_view reason;
  };
  const std::array cases = {
      Case{"missing version", {0, 1}, "WOFF2: invalid collection directory"},
      Case{"unsupported version", {0, 1, 0, 3, 0, 0, 1}, "WOFF2: invalid collection directory"},
      Case{"zero fonts", {0, 1, 0, 1, 0, 0, 0}, "WOFF2: invalid collection directory"},
      Case{"truncated 16-bit font count",
           {0, 1, 0, 1, 0, 0, 253, 1},
           "WOFF2: invalid collection directory"},
      Case{"truncated high font count",
           {0, 1, 0, 1, 0, 0, 254},
           "WOFF2: invalid collection directory"},
      Case{"truncated medium font count",
           {0, 1, 0, 1, 0, 0, 255},
           "WOFF2: invalid collection directory"},
      Case{"16-bit font count over limit",
           {0, 1, 0, 1, 0, 0, 253, 1, 1},
           "WOFF2: collection font count exceeds limit"},
      Case{"high font count over limit",
           {0, 1, 0, 1, 0, 0, 254, 0},
           "WOFF2: collection font count exceeds limit"},
      Case{"medium font count over limit",
           {0, 1, 0, 1, 0, 0, 255, 4},
           "WOFF2: collection font count exceeds limit"},
      Case{"zero tables in font", {0, 1, 0, 1, 0, 0, 1, 0}, "WOFF2: invalid collection directory"},
      Case{
          "truncated font flavor", {0, 1, 0, 1, 0, 0, 1, 1}, "WOFF2: invalid collection directory"},
      Case{"table references over limit",
           {0, 1, 0, 1, 0, 0, 1, 253, 0x40, 1, 0, 1, 0, 0},
           "WOFF2: collection table references exceed limit"},
      Case{"missing table index",
           {0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0},
           "WOFF2: invalid collection directory"},
      Case{"table index beyond directory",
           {0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1},
           "WOFF2: invalid collection directory"},
  };
  for (const Case& testCase : cases) {
    SCOPED_TRACE(testCase.scenario);
    const auto result =
        Woff2Parser::Decompress(Woff2PreflightInput(testCase.directory, kCollectionFlavor));
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().reason, testCase.reason);
  }
}

TEST(Woff2ParserTest, CompleteCollectionDirectoryPassesResourcePreflight) {
  constexpr uint32_t kCollectionFlavor = 0x74746366u;  // ttcf
  const std::array<uint8_t, 13> directory = {
      0, 1,        // One table with a one-byte original length.
      0, 1, 0, 0,  // TTC version 1.0.
      1,           // One font.
      1,           // One table reference.
      0, 1, 0, 0,  // TrueType flavor.
      0,           // Reference to table zero.
  };
  const auto result = Woff2Parser::Decompress(Woff2PreflightInput(directory, kCollectionFlavor));
  ASSERT_TRUE(result.hasError());  // The intentionally absent Brotli stream still fails decode.
  EXPECT_EQ(result.error().reason, "WOFF2: decompression failed");
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

TEST(Woff2ParserTest, ExactOutputAndScopedBrotliBudget) {
  const auto bytes = readFile("donner/base/fonts/testdata/valid-001.woff2");
  const auto generic = Woff2Parser::Decompress(bytes);
  ASSERT_EQ(generic.hasError(), false);
  Woff2Parser::Options options;
  options.expectedOutputSize = generic.result().size();
  options.maximumBrotliMemory = 8 * 1024 * 1024;
  const auto bounded = Woff2Parser::Decompress(bytes, options);
  ASSERT_EQ(bounded.hasError(), false);
  EXPECT_THAT(bounded.result(), testing::ElementsAreArray(generic.result()));
  options.maximumBrotliMemory = 1;
  const auto denied = Woff2Parser::Decompress(bytes, options);
  ASSERT_EQ(denied.hasError(), true);
  EXPECT_THAT(denied.error().reason, testing::Eq("WOFF2: bounded decompression failed"));
  // Failed instance admission cannot consume another decode's budget or affect the generic path.
  options.maximumBrotliMemory = 8 * 1024 * 1024;
  EXPECT_EQ(Woff2Parser::Decompress(bytes, options).hasError(), false);
  options.expectedOutputSize += 1;
  const auto wrongSize = Woff2Parser::Decompress(bytes, options);
  ASSERT_EQ(wrongSize.hasError(), true);
  EXPECT_THAT(wrongSize.error().reason,
              testing::Eq("WOFF2: declared size differs from expected output size"));
}

TEST(Woff2ParserTest, CatalogLimitsRejectIntermediateAndGlyfWorkBeforeDecode) {
  Woff2Parser::Options options;
  options.maximumIntermediateSize = 2 * 1024 * 1024;
  options.maximumTransformedGlyfSize = 512 * 1024;
  auto intermediate =
      Woff2Parser::Decompress(woff2WithIntermediateSize(2 * 1024 * 1024 + 1), options);
  ASSERT_EQ(intermediate.hasError(), true);
  EXPECT_THAT(intermediate.error().reason,
              testing::Eq("WOFF2: intermediate decompressed size exceeds limit"));
  auto glyf = Woff2Parser::Decompress(woff2WithTransformedGlyfSize(512 * 1024 + 1), options);
  ASSERT_EQ(glyf.hasError(), true);
  EXPECT_THAT(glyf.error().reason, testing::Eq("WOFF2: transformed glyf size exceeds limit"));
}

TEST(Woff2ParserTest, CatalogTableLimitDoesNotChangeTheGenericDefault) {
  auto bytes = woff2WithIntermediateSize(1);
  bytes[12] = 0;
  bytes[13] = 65;  // Below the generic limit, above the catalog limit.
  Woff2Parser::Options options;
  options.maximumTableCount = 64;
  const auto catalog = Woff2Parser::Decompress(bytes, options);
  ASSERT_EQ(catalog.hasError(), true);
  EXPECT_THAT(catalog.error().reason, testing::Eq("WOFF2: table count exceeds limit"));
  const auto generic = Woff2Parser::Decompress(bytes);
  ASSERT_EQ(generic.hasError(), true);
  EXPECT_THAT(generic.error().reason, testing::Ne("WOFF2: table count exceeds limit"));
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
