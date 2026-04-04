#include "donner/svg/text/TextBackendFull.h"

#include <gtest/gtest.h>

#include <fstream>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"

namespace donner::svg {

namespace {

FontHandle LoadResvgFont(FontManager& fontManager, const std::string& fontFilename,
                         const std::string& familyName) {
  const std::string fontsDir = Runfiles::instance().RlocationExternal("resvg-test-suite", "fonts");
  const std::string fontPath = fontsDir + "/" + fontFilename;

  std::ifstream file(fontPath, std::ios::binary);
  if (!file) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);

  auto fontData = std::make_shared<const std::vector<uint8_t>>(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(const_cast<uint8_t*>(fontData->data())), size);

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = fontData;

  css::FontFace face;
  face.familyName = RcString(familyName);
  face.sources.push_back(std::move(source));

  fontManager.addFontFace(face);
  return fontManager.findFont(RcString(familyName));
}

TEST(TextBackendFullTest, ShapeRunProducesGlyphsForLatinText) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend.shapeRun(font, 16.0f, "Hello", 0, 5, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 5u);

  uint32_t previousCluster = 0;
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    EXPECT_GT(shaped.glyphs[i].glyphIndex, 0);
    EXPECT_GT(shaped.glyphs[i].xAdvance, 0.0);
    if (i > 0) {
      EXPECT_GE(shaped.glyphs[i].cluster, previousCluster);
    }
    previousCluster = shaped.glyphs[i].cluster;
  }
}

TEST(TextBackendFullTest, VerticalLatinUsesSidewaysAdvances) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped = backend.shapeRun(font, 64.0f, "Text", 0, 4, true, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 4u);

  for (const auto& glyph : shaped.glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
    EXPECT_GT(glyph.xAdvance, 0.0);
    EXPECT_DOUBLE_EQ(glyph.yAdvance, 0.0);
    EXPECT_DOUBLE_EQ(glyph.xOffset, 0.0);
    EXPECT_DOUBLE_EQ(glyph.yOffset, 0.0);
  }
}

TEST(TextBackendFullTest, VerticalCjkUsesVerticalOffsets) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));

  const char* text = "\xE6\x97\xA5\xE6\x9C\xAC";  // 日本
  const auto shaped = backend.shapeRun(font, 64.0f, text, 0, 6, true, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 2u);

  for (const auto& glyph : shaped.glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
    EXPECT_GT(glyph.yAdvance, 0.0);
    EXPECT_GT(glyph.yOffset, 0.0);
  }
}

TEST(TextBackendFullTest, BitmapGlyphReturnsEmojiBitmap) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoColorEmoji.ttf", "Noto Color Emoji");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend.shapeRun(font, 32.0f, "\xF0\x9F\x98\x81", 0, 4, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 1u);
  ASSERT_GT(shaped.glyphs[0].glyphIndex, 0);

  const float scale = backend.scaleForEmToPixels(font, 32.0f);
  const auto bitmap = backend.bitmapGlyph(font, shaped.glyphs[0].glyphIndex, scale);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_GT(bitmap->width, 0);
  EXPECT_GT(bitmap->height, 0);
  EXPECT_EQ(bitmap->rgbaPixels.size(), static_cast<size_t>(bitmap->width * bitmap->height * 4));
  EXPECT_GT(bitmap->scale, 0.0);
}

TEST(TextBackendFullTest, ReportsCursiveAndSmallCapsCapabilities) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_TRUE(backend.isCursive(0x0627));  // Arabic alef
  EXPECT_FALSE(backend.isCursive('A'));
  EXPECT_TRUE(backend.hasSmallCapsFeature(font));
}

TEST(TextBackendFullTest, SynthesizedSmallCapsMarksLowercaseGlyphs) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_FALSE(backend.hasSmallCapsFeature(font));

  const auto shaped =
      backend.shapeRun(font, 32.0f, "aB", 0, 2, false, FontVariant::SmallCaps, false);
  ASSERT_EQ(shaped.glyphs.size(), 2u);

  EXPECT_LT(shaped.glyphs[0].fontSizeScale, 1.0f);
  EXPECT_FLOAT_EQ(shaped.glyphs[1].fontSizeScale, 1.0f);
}

}  // namespace

}  // namespace donner::svg
