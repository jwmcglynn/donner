#include "donner/svg/resources/FontManager.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

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

  const stbtt_fontinfo* info = mgr.fontInfo(handle);
  ASSERT_NE(info, nullptr);

  // Verify font metrics are reasonable.
  int ascent = 0;
  int descent = 0;
  int lineGap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
  EXPECT_GT(ascent, 0);
  EXPECT_LT(descent, 0);  // descent is typically negative
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

  const stbtt_fontinfo* info = mgr.fontInfo(handle);
  ASSERT_NE(info, nullptr);

  // Verify that we can look up a glyph for 'A'.
  int glyphIndex = stbtt_FindGlyphIndex(info, 'A');
  EXPECT_GT(glyphIndex, 0);

  // Verify advance width is positive.
  int advanceWidth = 0;
  int leftSideBearing = 0;
  stbtt_GetGlyphHMetrics(info, glyphIndex, &advanceWidth, &leftSideBearing);
  EXPECT_GT(advanceWidth, 0);
}

TEST(FontManagerTest, LoadWoff1Data) {
  FontManager mgr;

  std::vector<uint8_t> woffData = readFile("donner/base/fonts/testdata/valid-001.woff");
  ASSERT_FALSE(woffData.empty()) << "Could not read WOFF test file";

  FontHandle handle = mgr.loadFontData(woffData);
  EXPECT_TRUE(static_cast<bool>(handle));

  const stbtt_fontinfo* info = mgr.fontInfo(handle);
  ASSERT_NE(info, nullptr);

  // Verify font metrics are readable (the test font is "WOFF Test CFF" and may not have
  // standard Latin glyphs, but stb_truetype should parse it successfully).
  int ascent = 0;
  int descent = 0;
  int lineGap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
  // CFF fonts should report valid vertical metrics.
  EXPECT_NE(ascent, 0);
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

TEST(FontManagerTest, FontInfoReturnsNullForInvalidHandle) {
  FontManager mgr;
  FontHandle invalid;
  EXPECT_EQ(mgr.fontInfo(invalid), nullptr);
  EXPECT_EQ(mgr.scaleForPixelHeight(invalid, 16.0f), 0.0f);
  EXPECT_TRUE(mgr.fontData(invalid).empty());
}

TEST(FontManagerTest, ScaleForPixelHeight) {
  FontManager mgr;
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  float scale = mgr.scaleForPixelHeight(handle, 16.0f);
  EXPECT_GT(scale, 0.0f);

  // Larger pixel height should give a larger scale factor.
  float scaleLarge = mgr.scaleForPixelHeight(handle, 32.0f);
  EXPECT_GT(scaleLarge, scale);
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
  source.payload =
      std::vector<uint8_t>(embedded::kPublicSansMediumOtf.begin(),
                           embedded::kPublicSansMediumOtf.end());
  face.sources.push_back(std::move(source));

  mgr.addFontFace(face);

  FontHandle handle = mgr.findFont("TestFont");
  EXPECT_TRUE(static_cast<bool>(handle));

  // The loaded font should be different from the fallback (separate allocation).
  FontHandle fallback = mgr.fallbackFont();
  EXPECT_NE(handle, fallback);

  // But both should work.
  const stbtt_fontinfo* testInfo = mgr.fontInfo(handle);
  ASSERT_NE(testInfo, nullptr);
  EXPECT_GT(stbtt_FindGlyphIndex(testInfo, 'A'), 0);
}

TEST(FontManagerTest, GlyphShapeExtraction) {
  FontManager mgr;
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  const stbtt_fontinfo* info = mgr.fontInfo(handle);
  ASSERT_NE(info, nullptr);

  int glyphIndex = stbtt_FindGlyphIndex(info, 'A');
  ASSERT_GT(glyphIndex, 0);

  stbtt_vertex* vertices = nullptr;
  int numVertices = stbtt_GetGlyphShape(info, glyphIndex, &vertices);
  EXPECT_GT(numVertices, 0);
  EXPECT_NE(vertices, nullptr);

  // Verify we got valid vertex types.
  for (int i = 0; i < numVertices; ++i) {
    EXPECT_TRUE(vertices[i].type == STBTT_vmove || vertices[i].type == STBTT_vline ||
                vertices[i].type == STBTT_vcurve || vertices[i].type == STBTT_vcubic);
  }

  stbtt_FreeShape(info, vertices);
}

TEST(FontManagerTest, KernAdvance) {
  FontManager mgr;
  FontHandle handle = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(handle));

  const stbtt_fontinfo* info = mgr.fontInfo(handle);
  ASSERT_NE(info, nullptr);

  int glyphA = stbtt_FindGlyphIndex(info, 'A');
  int glyphV = stbtt_FindGlyphIndex(info, 'V');
  ASSERT_GT(glyphA, 0);
  ASSERT_GT(glyphV, 0);

  // AV is a commonly kerned pair — the kern advance may be negative or zero depending
  // on the font, but the call should not crash.
  int kern = stbtt_GetGlyphKernAdvance(info, glyphA, glyphV);
  (void)kern;  // Just verify no crash; actual value is font-dependent.
}

}  // namespace donner::svg
