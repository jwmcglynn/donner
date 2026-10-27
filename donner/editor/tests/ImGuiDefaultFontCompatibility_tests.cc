#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "donner/base/PackagedResource.h"
#include "donner/base/tests/Runfiles.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "imgui.h"

namespace donner {
namespace {

std::vector<uint8_t> RawFont() {
  std::ifstream input(Runfiles::instance().Rlocation("third_party/imgui/proggy_fixture.ttf"),
                      std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void AddRawDefault(ImFontAtlas& atlas, const std::vector<uint8_t>& bytes,
                   const ImFontConfig* settings) {
  ImFontConfig config = settings ? *settings : ImFontConfig();
  if (!settings) {
    config.OversampleH = config.OversampleV = 1;
    config.PixelSnapH = true;
  }
  if (config.SizePixels <= 0.0f) {
    config.SizePixels = 13.0f;
  }
  if (config.Name[0] == '\0') {
    std::snprintf(config.Name, sizeof(config.Name), "ProggyClean.ttf, %dpx",
                  static_cast<int>(config.SizePixels));
  }
  config.EllipsisChar = 0x0085;
  config.GlyphOffset.y = static_cast<float>(static_cast<int>(config.SizePixels / 13.0f));
  config.FontDataOwnedByAtlas = false;
  atlas.AddFontFromMemoryTTF(
      const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size()), config.SizePixels,
      &config, config.GlyphRanges ? config.GlyphRanges : atlas.GetGlyphRangesDefault());
}

void ExpectSamePixels(ImFontAtlas& actual, ImFontAtlas& expected) {
  unsigned char* actualPixels = nullptr;
  unsigned char* expectedPixels = nullptr;
  int actualWidth = 0, actualHeight = 0, expectedWidth = 0, expectedHeight = 0;
  actual.GetTexDataAsAlpha8(&actualPixels, &actualWidth, &actualHeight);
  expected.GetTexDataAsAlpha8(&expectedPixels, &expectedWidth, &expectedHeight);
  ASSERT_NE(actualPixels, nullptr);
  ASSERT_NE(expectedPixels, nullptr);
  ASSERT_EQ(actualWidth, expectedWidth);
  ASSERT_EQ(actualHeight, expectedHeight);
  const size_t count = static_cast<size_t>(actualWidth) * actualHeight;
  EXPECT_THAT(std::vector<uint8_t>(actualPixels, actualPixels + count),
              testing::ElementsAreArray(expectedPixels, count));
  ASSERT_EQ(actual.Fonts.Size, expected.Fonts.Size);
  EXPECT_FLOAT_EQ(actual.Fonts[0]->FontSize, expected.Fonts[0]->FontSize);
  EXPECT_EQ(actual.Fonts[0]->Glyphs.Size, expected.Fonts[0]->Glyphs.Size);
}

TEST(ImGuiDefaultFontCompatibility, PinnedRawFontMatchesTheOriginalCompleteDefault) {
  const auto bytes = RawFont();
  ASSERT_EQ(bytes.size(), 41208u);
  EXPECT_EQ(ResourceSha256(bytes),
            "527d2a443ce051f93f7e77b855609722b8cb220a9f104b4aa037be5c90b71324");
  ImFontAtlas actual, expected;
  ASSERT_NE(actual.AddFontDefault(), nullptr);
  AddRawDefault(expected, bytes, nullptr);
  ExpectSamePixels(actual, expected);
}

TEST(ImGuiDefaultFontCompatibility, EmptyAtlasBuildStillLoadsTheSameDefaultFont) {
  const auto bytes = RawFont();
  ASSERT_EQ(bytes.size(), 41208u);
  ImFontAtlas actual, expected;
  ASSERT_TRUE(actual.Build());
  AddRawDefault(expected, bytes, nullptr);
  ExpectSamePixels(actual, expected);
}

TEST(ImGuiDefaultFontCompatibility, CustomSizeRangesAndConfigurationPreservePixels) {
  const auto bytes = RawFont();
  ASSERT_EQ(bytes.size(), 41208u);
  static const ImWchar ranges[] = {'A', 'Z', 0};
  ImFontConfig config;
  config.SizePixels = 26.0f;
  config.OversampleH = 2;
  config.OversampleV = 1;
  config.PixelSnapH = false;
  config.GlyphOffset.y = 99.0f;
  config.GlyphRanges = ranges;
  config.FontDataOwnedByAtlas = false;
  std::snprintf(config.Name, sizeof(config.Name), "custom default");
  ImFontAtlas actual, expected;
  ASSERT_NE(actual.AddFontDefault(&config), nullptr);
  ASSERT_EQ(actual.ConfigData.Size, 1);
  EXPECT_TRUE(actual.ConfigData[0].FontDataOwnedByAtlas);
  EXPECT_STREQ(actual.ConfigData[0].Name, "custom default");
  EXPECT_FLOAT_EQ(actual.ConfigData[0].GlyphOffset.y, 2.0f);
  AddRawDefault(expected, bytes, &config);
  ExpectSamePixels(actual, expected);
}

}  // namespace
}  // namespace donner
