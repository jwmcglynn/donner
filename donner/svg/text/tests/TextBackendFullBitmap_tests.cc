#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
