#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/BitmapGlyphUtils.h"
#include "donner/svg/text/TextBackendFull.h"

namespace donner::svg {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Gt;
using ::testing::Not;
using ::testing::Optional;
using ::testing::SizeIs;

std::vector<uint8_t> LoadBitmapFontSeed() {
  const std::string path = Runfiles::instance().Rlocation(
      "donner/svg/text/tests/font_decoder_corpus/bitmap_emoji_subset.ttf");
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

std::vector<uint8_t> MonochromeBitmapFontSeed() {
  auto bytes = LoadBitmapFontSeed();
  if (bytes.size() < 12) {
    return {};
  }
  const auto read32 = [&bytes](std::size_t at) {
    return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) |
           (std::uint32_t(bytes[at + 2]) << 8) | std::uint32_t(bytes[at + 3]);
  };
  const auto write32 = [&bytes](std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[at + i] = static_cast<uint8_t>(value >> (24 - i * 8));
    }
  };
  const std::size_t tables = (std::size_t(bytes[4]) << 8) | bytes[5];
  if (tables > (bytes.size() - 12) / 16) {
    return {};
  }
  for (std::size_t i = 0; i < tables; ++i) {
    const std::size_t record = 12 + i * 16;
    const std::string_view tag(reinterpret_cast<const char*>(bytes.data() + record), 4);
    const std::size_t offset = read32(record + 8);
    if (offset > bytes.size() || bytes.size() - offset < 80) {
      continue;
    }
    if (tag == "CBDT") {
      bytes[record] = 'E';
      write32(offset, 0x00020000);
      const std::array<uint8_t, 6> bitmap = {1, 1, 0, 1, 1, 0x80};
      std::copy(bitmap.begin(), bitmap.end(), bytes.begin() + offset + 4);
      write32(record + 12, 10);
    } else if (tag == "CBLC") {
      bytes[record] = 'E';
      write32(offset, 0x00020000);
      bytes[offset + 54] = 1;
      bytes[offset + 66] = 0;
      bytes[offset + 67] = 1;
      write32(offset + 76, 6);
    }
  }
  return bytes;
}

auto GlyphIndexIs(auto matcher) {
  return Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, matcher);
}

TEST(BitmapGlyphUtils, AcceptsCheckedPositiveAndNegativePitchLayouts) {
  std::array<uint8_t, 32> buffer{};

  EXPECT_THAT(
      details::ValidateBgraBitmapLayout(2, 2, 12, buffer.data()),
      Optional(AllOf(Field("width", &details::BgraBitmapLayout::width, 2),
                     Field("height", &details::BgraBitmapLayout::height, 2),
                     Field("pitch", &details::BgraBitmapLayout::pitch, 12),
                     Field("rowBytes", &details::BgraBitmapLayout::rowBytes, 8),
                     Field("sourceSpanBytes", &details::BgraBitmapLayout::sourceSpanBytes, 20),
                     Field("rgbaBytes", &details::BgraBitmapLayout::rgbaBytes, 16))));
  EXPECT_THAT(details::ValidateBgraBitmapLayout(2, 2, -12, buffer.data()),
              Optional(Field("pitch", &details::BgraBitmapLayout::pitch, -12)));
}

TEST(BitmapGlyphUtils, RejectsMalformedAndUnreasonableLayouts) {
  std::array<uint8_t, 16> buffer{};

  EXPECT_EQ(details::ValidateBgraBitmapLayout(1, 1, 4, nullptr), std::nullopt);
  EXPECT_EQ(details::ValidateBgraBitmapLayout(0, 1, 4, buffer.data()), std::nullopt);
  EXPECT_EQ(details::ValidateBgraBitmapLayout(1, 0, 4, buffer.data()), std::nullopt);
  EXPECT_EQ(details::ValidateBgraBitmapLayout(2, 1, 7, buffer.data()), std::nullopt);
  EXPECT_EQ(details::ValidateBgraBitmapLayout(details::kMaximumBitmapGlyphDimension + 1, 1, 4,
                                              buffer.data()),
            std::nullopt);
  EXPECT_EQ(details::ValidateBgraBitmapLayout(1, 2, std::numeric_limits<int>::min(), buffer.data()),
            std::nullopt);
  EXPECT_EQ(details::ValidatedBgraOutputBytes(details::kMaximumBitmapGlyphDimension + 1, 1),
            std::nullopt);
  EXPECT_EQ(details::ValidatedBgraOutputBytes(1, details::kMaximumBitmapGlyphDimension + 1),
            std::nullopt);

  const unsigned int maximumDimension =
      static_cast<unsigned int>(details::kMaximumBitmapGlyphDimension);
  const int tightlyPackedPitch = static_cast<int>(maximumDimension * 4);
  EXPECT_THAT(details::ValidateBgraBitmapLayout(maximumDimension, maximumDimension,
                                                tightlyPackedPitch, buffer.data()),
              Optional(Field("rgbaBytes", &details::BgraBitmapLayout::rgbaBytes,
                             details::kMaximumBitmapGlyphBytes)));
  EXPECT_EQ(details::ValidateBgraBitmapLayout(maximumDimension, maximumDimension,
                                              tightlyPackedPitch + 1, buffer.data()),
            std::nullopt);
}

TEST(BitmapGlyphUtils, ConvertsNegativePitchRowsInLogicalOrder) {
  std::array<uint8_t, 20> storage{
      9, 10, 11, 12, 13, 14, 15, 16, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8,
  };
  const uint8_t* rawBuffer = storage.data();
  const auto layout =
      details::ValidateBgraBitmapLayout(/*width=*/2, /*rows=*/2, /*pitch=*/-12, rawBuffer);
  ASSERT_TRUE(layout.has_value());

  EXPECT_THAT(details::ConvertValidatedBgraToRgba(rawBuffer, *layout),
              ElementsAre(3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12, 15, 14, 13, 16));
}

TEST(TextBackendFullBitmap, DecodesBoundedTrustedBitmapFontSeed) {
  const std::vector<uint8_t> fontBytes = LoadBitmapFontSeed();
  ASSERT_THAT(fontBytes, Not(SizeIs(0)));

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = fontManager.loadFontData(fontBytes, FontDataTrust::Trusted);
  ASSERT_TRUE(static_cast<bool>(font));

  TextBackendFull backend(fontManager, registry);
  constexpr std::string_view kEmoji = "\xF0\x9F\x98\x81";
  const TextBackend::ShapedRun shaped =
      backend.shapeRun(font, 32.0f, kEmoji, 0, kEmoji.size(), false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));

  const float scale = backend.scaleForEmToPixels(font, 32.0f);
  const auto bitmap = backend.bitmapGlyph(font, shaped.glyphs.front().glyphIndex, scale);
  ASSERT_THAT(bitmap, Optional(AllOf(Field("width", &TextBackend::BitmapGlyph::width, 136),
                                     Field("height", &TextBackend::BitmapGlyph::height, 128))));
  EXPECT_THAT(bitmap->rgbaPixels, SizeIs(136 * 128 * 4));
}

TEST(TextBackendFullBitmap, QueriesMetadataAndHonorsExactDecodeByteAdmission) {
  Registry registry;
  FontManager manager(registry);
  const FontHandle font = manager.loadFontData(LoadBitmapFontSeed(), FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  TextBackendFull backend(manager, registry);
  const TextBackend& api = backend;
  constexpr std::string_view kEmoji = "\xF0\x9F\x98\x81";
  const auto shaped =
      backend.shapeRun(font, 32.0f, kEmoji, 0, kEmoji.size(), false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));
  const int glyphIndex = shaped.glyphs.front().glyphIndex;
  const float scale = backend.scaleForEmToPixels(font, 32.0f);
  const auto metrics = api.bitmapGlyphMetrics(font, glyphIndex, scale);
  ASSERT_THAT(metrics,
              Optional(AllOf(Field(&TextBackend::BitmapGlyphMetrics::width, 136),
                             Field(&TextBackend::BitmapGlyphMetrics::height, 128),
                             Field(&TextBackend::BitmapGlyphMetrics::rgbaBytes, 136 * 128 * 4))));
  const auto bitmap = api.bitmapGlyph(font, glyphIndex, scale, *metrics, metrics->rgbaBytes);
  ASSERT_THAT(bitmap,
              Optional(Field(&TextBackend::BitmapGlyph::rgbaPixels, SizeIs(metrics->rgbaBytes))));
  EXPECT_THAT(api.bitmapGlyph(font, glyphIndex, scale, *metrics, metrics->rgbaBytes - 1),
              testing::Eq(std::nullopt));
  EXPECT_THAT(api.bitmapGlyph(font, glyphIndex, scale, *metrics, 0), testing::Eq(std::nullopt));

  std::array<TextBackend::BitmapGlyphMetrics, 10> forged;
  forged.fill(*metrics);
  ++forged[0].width;
  ++forged[1].height;
  forged[2].rgbaBytes = 0;
  forged[3].font = manager.fallbackFont();
  ++forged[4].glyphIndex;
  ++forged[5].selectedStrikeIndex;
  ++forged[6].bearingX;
  ++forged[7].bearingY;
  forged[8].scale = std::numeric_limits<double>::quiet_NaN();
  forged[9].requestedScale = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < forged.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_THAT(api.bitmapGlyph(font, glyphIndex, scale, forged[i], metrics->rgbaBytes),
                testing::Eq(std::nullopt));
  }
  EXPECT_THAT(api.bitmapGlyph(font, glyphIndex, scale * 2, *metrics, metrics->rgbaBytes),
              testing::Eq(std::nullopt));
}

TEST(TextBackendFullBitmap, MonochromeEmbeddedBitmapDoesNotAdvertiseBgraAdmission) {
  Registry registry;
  FontManager manager(registry);
  const auto monochromeBytes = MonochromeBitmapFontSeed();
  ASSERT_THAT(monochromeBytes, Not(SizeIs(0)));
  const FontHandle monochrome = manager.loadFontData(monochromeBytes, FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(monochrome), testing::IsTrue());
  TextBackendFull backend(manager, registry);
  ASSERT_THAT(backend.isBitmapOnly(monochrome), testing::IsTrue());
  const float scale = backend.scaleForEmToPixels(monochrome, 32.0f);
  EXPECT_THAT(backend.bitmapGlyph(monochrome, 1, scale), testing::Eq(std::nullopt));
  EXPECT_THAT(backend.bitmapGlyphMetrics(monochrome, 1, scale), testing::Eq(std::nullopt))
      << "A real one-bit EBDT glyph must be rejected before claiming BGRA cost.";
  const FontHandle color = manager.loadFontData(LoadBitmapFontSeed(), FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(color), testing::IsTrue());
  const auto colorMetrics = backend.bitmapGlyphMetrics(color, 1, scale);
  ASSERT_THAT(colorMetrics,
              Optional(Field(&TextBackend::BitmapGlyphMetrics::rgbaBytes, 136 * 128 * 4)));
  EXPECT_THAT(
      backend.bitmapGlyph(color, 1, scale, *colorMetrics, colorMetrics->rgbaBytes),
      Optional(Field(&TextBackend::BitmapGlyph::rgbaPixels, SizeIs(colorMetrics->rgbaBytes))));
}

TEST(TextBackendFullBitmap, MetadataRejectsUntrustedFontsAndInvalidRequests) {
  Registry registry;
  FontManager manager(registry);
  const auto bytes = LoadBitmapFontSeed();
  const FontHandle untrusted = manager.loadFontData(bytes, FontDataTrust::Untrusted);
  const FontHandle trusted = manager.loadFontData(bytes, FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(untrusted), testing::IsTrue());
  ASSERT_THAT(static_cast<bool>(trusted), testing::IsTrue());
  ASSERT_THAT(manager.isTrustedFont(trusted), testing::IsTrue());
  ASSERT_THAT(manager.isTrustedFont(untrusted), testing::IsFalse());
  TextBackendFull backend(manager, registry);
  const TextBackend& api = backend;
  EXPECT_THAT(api.bitmapGlyphMetrics(untrusted, 1, 1.0f), testing::Eq(std::nullopt));
  EXPECT_THAT(api.bitmapGlyphMetrics(FontHandle(), 1, 1.0f), testing::Eq(std::nullopt));
  EXPECT_THAT(api.bitmapGlyphMetrics(trusted, -1, 1.0f), testing::Eq(std::nullopt));
  for (float invalidScale : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity()}) {
    SCOPED_TRACE(invalidScale);
    EXPECT_THAT(api.bitmapGlyphMetrics(trusted, 1, invalidScale), testing::Eq(std::nullopt));
  }
}

TEST(TextBackendFullBitmap, RejectsInvalidDecodeRequestsBeforeLoadingGlyph) {
  const std::vector<uint8_t> fontBytes = LoadBitmapFontSeed();
  ASSERT_THAT(fontBytes, Not(SizeIs(0)));

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = fontManager.loadFontData(fontBytes, FontDataTrust::Trusted);
  ASSERT_TRUE(static_cast<bool>(font));

  TextBackendFull backend(fontManager, registry);
  EXPECT_EQ(backend.bitmapGlyph(font, -1, 1.0f), std::nullopt);
  EXPECT_EQ(backend.bitmapGlyph(font, 1, 0.0f), std::nullopt);
  EXPECT_EQ(backend.bitmapGlyph(font, 1, std::numeric_limits<float>::infinity()), std::nullopt);
  EXPECT_EQ(backend.bitmapGlyph(font, 1, std::numeric_limits<float>::quiet_NaN()), std::nullopt);
}

}  // namespace
}  // namespace donner::svg
