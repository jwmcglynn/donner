#include "donner/svg/resources/FontManager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

namespace {

/// Read a file from disk into a byte vector.
std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(FontManagerTest, FallbackFontLoads) {
  FontManager mgr;
  FontHandle handle = mgr.fallbackFont();
  EXPECT_TRUE(static_cast<bool>(handle));

  auto data = mgr.fontData(handle);
  EXPECT_FALSE(data.empty());
}

TEST(FontManagerTest, FallbackFontIsCached) {
  FontManager mgr;
  FontHandle h1 = mgr.fallbackFont();
  FontHandle h2 = mgr.fallbackFont();
  EXPECT_EQ(h1, h2);
}

TEST(FontManagerTest, LoadRawOtfData) {
  FontManager mgr;

  // Load the embedded Public Sans font data directly (raw OTF).
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  FontHandle handle = mgr.loadFontData(data);
  EXPECT_TRUE(static_cast<bool>(handle));

  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, LoadWoff1Data) {
  FontManager mgr;

  std::vector<uint8_t> woffData = readFile("donner/base/fonts/testdata/valid-001.woff");
  ASSERT_FALSE(woffData.empty()) << "Could not read WOFF test file";

  FontHandle handle = mgr.loadFontData(woffData);
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(mgr.fontData(handle).empty());
}

TEST(FontManagerTest, InvalidDataReturnsInvalidHandle) {
  FontManager mgr;

  // Empty data.
  FontHandle h1 = mgr.loadFontData({});
  EXPECT_FALSE(static_cast<bool>(h1));

  // Garbage data.
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  FontHandle h2 = mgr.loadFontData(garbage);
  EXPECT_FALSE(static_cast<bool>(h2));
}

TEST(FontManagerTest, InvalidHandleReturnsEmptyMeasurements) {
  FontManager mgr;
  FontHandle invalid;
  EXPECT_TRUE(mgr.fontData(invalid).empty());
}

TEST(FontManagerTest, FontDataReturnsNonEmpty) {
  FontManager mgr;
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  auto data = mgr.fontData(handle);
  EXPECT_FALSE(data.empty());
}

TEST(FontManagerTest, FindFontFallsBackToPublicSans) {
  FontManager mgr;

  // No @font-face registered, so any family name should fall back.
  FontHandle handle = mgr.findFont("NonExistentFont");
  EXPECT_TRUE(static_cast<bool>(handle));

  // Should be the same as the fallback.
  FontHandle fallback = mgr.fallbackFont();
  EXPECT_EQ(handle, fallback);
}

TEST(FontManagerTest, FindFontCachesResult) {
  FontManager mgr;

  FontHandle h1 = mgr.findFont("Anything");
  FontHandle h2 = mgr.findFont("Anything");
  EXPECT_EQ(h1, h2);
}

TEST(FontManagerTest, AddFontFaceWithDataSource) {
  FontManager mgr;

  // Create a @font-face with inline data from the embedded Public Sans font.
  css::FontFace face;
  face.familyName = RcString("TestFont");

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(
      embedded::kPublicSansMediumOtf.begin(), embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));

  mgr.addFontFace(face);

  FontHandle handle = mgr.findFont("TestFont");
  EXPECT_TRUE(static_cast<bool>(handle));

  // The loaded font should be different from the fallback (separate allocation).
  FontHandle fallback = mgr.fallbackFont();
  EXPECT_NE(handle, fallback);

  EXPECT_FALSE(mgr.fontData(handle).empty());
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
TEST(FontManagerTest, LoadWoff2Data) {
  FontManager mgr;

  std::vector<uint8_t> woff2Data = readFile("donner/base/fonts/testdata/valid-001.woff2");
  ASSERT_FALSE(woff2Data.empty()) << "Could not read WOFF2 test file";

  FontHandle handle = mgr.loadFontData(woff2Data);
  EXPECT_TRUE(static_cast<bool>(handle));
  EXPECT_FALSE(mgr.fontData(handle).empty());
}
#endif

}  // namespace donner::svg
