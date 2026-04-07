#include <gtest/gtest.h>

#include <fstream>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextBackendSimple.h"

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

// ── Backend selection ───────────────────────────────────────────────────────

enum class BackendType { Simple, Full };

std::string BackendName(const testing::TestParamInfo<BackendType>& info) {
  return info.param == BackendType::Simple ? "Simple" : "Full";
}

class TextBackendTest : public testing::TestWithParam<BackendType> {
protected:
  void SetUp() override {
    if (GetParam() == BackendType::Simple) {
      backend_ = std::make_unique<TextBackendSimple>(fontManager_, registry_);
    } else {
      backend_ = std::make_unique<TextBackendFull>(fontManager_, registry_);
    }
  }

  bool isSimple() const { return GetParam() == BackendType::Simple; }
  bool isFull() const { return GetParam() == BackendType::Full; }

  TextBackend& backend() { return *backend_; }

  FontHandle fallbackFont() { return fontManager_.fallbackFont(); }

  FontHandle loadFont(const std::string& filename, const std::string& family) {
    return LoadResvgFont(fontManager_, filename, family);
  }

  Registry registry_;
  FontManager fontManager_{registry_};
  std::unique_ptr<TextBackend> backend_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Shared contract tests — both backends must satisfy these.
// ═══════════════════════════════════════════════════════════════════════════

// ── Font metrics ────────────────────────────────────────────────────────────

TEST_P(TextBackendTest, FontVMetricsReturnsNonZeroValues) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const FontVMetrics metrics = backend().fontVMetrics(font);
  EXPECT_GT(metrics.ascent, 0);
  EXPECT_LT(metrics.descent, 0);
}

TEST_P(TextBackendTest, FontVMetricsReturnsZeroForInvalidFont) {
  const FontVMetrics metrics = backend().fontVMetrics(FontHandle{});
  EXPECT_EQ(metrics.ascent, 0);
  EXPECT_EQ(metrics.descent, 0);
  EXPECT_EQ(metrics.lineGap, 0);
}

TEST_P(TextBackendTest, ScaleForPixelHeightIsPositive) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_GT(backend().scaleForPixelHeight(font, 16.0f), 0.0f);
}

TEST_P(TextBackendTest, ScaleForEmToPixelsIsPositive) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_GT(backend().scaleForEmToPixels(font, 16.0f), 0.0f);
}

// ── Table-derived metrics ───────────────────────────────────────────────────

TEST_P(TextBackendTest, UnderlineMetricsPresent) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto underline = backend().underlineMetrics(font);
  ASSERT_TRUE(underline.has_value());
  EXPECT_NE(underline->position, 0.0);
  EXPECT_GT(underline->thickness, 0.0);
}

TEST_P(TextBackendTest, UnderlineMetricsNulloptForInvalidFont) {
  EXPECT_FALSE(backend().underlineMetrics(FontHandle{}).has_value());
}

TEST_P(TextBackendTest, SubSuperMetricsPresent) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto subSuper = backend().subSuperMetrics(font);
  ASSERT_TRUE(subSuper.has_value());
  EXPECT_GT(subSuper->subscriptYOffset, 0);
  EXPECT_GT(subSuper->superscriptYOffset, 0);
}

// ── Glyph outlines ──────────────────────────────────────────────────────────

TEST_P(TextBackendTest, GlyphOutlineProducesNonEmptyPath) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 16.0f, "A", 0, 1, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 1u);
  ASSERT_GT(shaped.glyphs[0].glyphIndex, 0);

  const float scale = backend().scaleForEmToPixels(font, 16.0f);
  const Path path = backend().glyphOutline(font, shaped.glyphs[0].glyphIndex, scale);
  EXPECT_FALSE(path.empty());
  EXPECT_GT(path.commands().size(), 0u);
}

TEST_P(TextBackendTest, GlyphOutlineEmptyForInvalidFont) {
  const Path path = backend().glyphOutline(FontHandle{}, 1, 1.0f);
  EXPECT_TRUE(path.empty());
}

TEST_P(TextBackendTest, IsBitmapOnlyFalseForOutlineFont) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_FALSE(backend().isBitmapOnly(font));
}

// ── Shaping ─────────────────────────────────────────────────────────────────

TEST_P(TextBackendTest, ShapeRunProducesGlyphsForLatinText) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 16.0f, "Hello", 0, 5, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 5u);

  uint32_t previousCluster = 0;
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    EXPECT_GT(shaped.glyphs[i].glyphIndex, 0);
    EXPECT_GT(shaped.glyphs[i].xAdvance, 0.0);
    EXPECT_FLOAT_EQ(shaped.glyphs[i].fontSizeScale, 1.0f);
    if (i > 0) {
      EXPECT_GE(shaped.glyphs[i].cluster, previousCluster);
    }
    previousCluster = shaped.glyphs[i].cluster;
  }
}

TEST_P(TextBackendTest, ShapeRunHandlesSubstring) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  // Shape only "ll" from "Hello" (bytes 2-4).
  const auto shaped =
      backend().shapeRun(font, 16.0f, "Hello", 2, 2, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 2u);

  // Both glyphs are 'l' — same glyph index.
  EXPECT_EQ(shaped.glyphs[0].glyphIndex, shaped.glyphs[1].glyphIndex);
  // HarfBuzz GPOS may adjust advances slightly per context, so check near-equality.
  EXPECT_NEAR(shaped.glyphs[0].xAdvance, shaped.glyphs[1].xAdvance, 1.0);
}

TEST_P(TextBackendTest, ShapeRunEmptyForInvalidFont) {
  const auto shaped =
      backend().shapeRun(FontHandle{}, 16.0f, "Hello", 0, 5, false, FontVariant::Normal, false);
  EXPECT_TRUE(shaped.glyphs.empty());
}

TEST_P(TextBackendTest, ShapeRunClustersMatchByteOffsets) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 16.0f, "ABC", 0, 3, false, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 3u);

  EXPECT_EQ(shaped.glyphs[0].cluster, 0u);
  EXPECT_EQ(shaped.glyphs[1].cluster, 1u);
  EXPECT_EQ(shaped.glyphs[2].cluster, 2u);
}

TEST_P(TextBackendTest, ShapeRunVerticalLatinUsesSidewaysAdvances) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 64.0f, "Text", 0, 4, true, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 4u);

  for (const auto& glyph : shaped.glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
    // Both backends report sideways Latin with xAdvance>0.
    // Simple sets yAdvance = xAdvance and xAdvance = 0; Full keeps xAdvance > 0 with yAdvance = 0.
    // The common contract: at least one of x/yAdvance is positive.
    EXPECT_GT(glyph.xAdvance + glyph.yAdvance, 0.0);
  }
}

TEST_P(TextBackendTest, ShapeRunVerticalCjk) {
  const FontHandle font = loadFont("MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));

  const char* text = "\xE6\x97\xA5\xE6\x9C\xAC";  // 日本
  const auto shaped =
      backend().shapeRun(font, 64.0f, text, 0, 6, true, FontVariant::Normal, false);
  ASSERT_EQ(shaped.glyphs.size(), 2u);

  for (const auto& glyph : shaped.glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
    EXPECT_GT(glyph.yAdvance, 0.0);
  }

  if (isSimple()) {
    // Simple backend: CJK vertical advance = font size (em height fallback).
    for (const auto& glyph : shaped.glyphs) {
      EXPECT_DOUBLE_EQ(glyph.xAdvance, 0.0);
      EXPECT_DOUBLE_EQ(glyph.yAdvance, 64.0);
    }
  } else {
    // Full backend: CJK vertical advance from font's vmtx/vhea tables, with y offsets.
    for (const auto& glyph : shaped.glyphs) {
      EXPECT_GT(glyph.yOffset, 0.0);
    }
  }
}

TEST_P(TextBackendTest, SmallCapsSynthesizesScaleForFontWithoutSmcp) {
  // MPLUS 1p doesn't have native smcp — both backends synthesize.
  const FontHandle font = loadFont("MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_FALSE(backend().hasSmallCapsFeature(font));

  const auto shaped =
      backend().shapeRun(font, 32.0f, "aB", 0, 2, false, FontVariant::SmallCaps, false);
  ASSERT_EQ(shaped.glyphs.size(), 2u);

  EXPECT_LT(shaped.glyphs[0].fontSizeScale, 1.0f);
  EXPECT_FLOAT_EQ(shaped.glyphs[1].fontSizeScale, 1.0f);
}

// ── Cross-span kerning ──────────────────────────────────────────────────────

TEST_P(TextBackendTest, CrossSpanKernReturnsZeroForInvalidFont) {
  EXPECT_DOUBLE_EQ(
      backend().crossSpanKern(FontHandle{}, 16.0f, FontHandle{}, 16.0f, 'A', 'V', false), 0.0);
}

TEST_P(TextBackendTest, CrossSpanKernReturnsFiniteValue) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const double kern = backend().crossSpanKern(font, 16.0f, font, 16.0f, 'A', 'V', false);
  EXPECT_TRUE(std::isfinite(kern));
}

// ── .notdef glyph ───────────────────────────────────────────────────────────

TEST_P(TextBackendTest, GlyphOutlineForNotdefDoesNotCrash) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const float scale = backend().scaleForEmToPixels(font, 16.0f);
  // Glyph index 0 is .notdef — may or may not have an outline. Just verify no crash.
  const Path path = backend().glyphOutline(font, 0, scale);
  EXPECT_GE(path.commands().size(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, TextBackendTest,
                         testing::Values(BackendType::Simple, BackendType::Full), BackendName);

// ═══════════════════════════════════════════════════════════════════════════
// Capability tests — behaviour that differs between backends.
// ═══════════════════════════════════════════════════════════════════════════

// ── Simple backend: no advanced capabilities ────────────────────────────────

TEST(TextBackendSimpleCapabilities, IsCursiveAlwaysFalse) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);

  EXPECT_FALSE(backend.isCursive('A'));
  EXPECT_FALSE(backend.isCursive(0x0627));  // Arabic alef
  EXPECT_FALSE(backend.isCursive(0x4E00));  // CJK
}

TEST(TextBackendSimpleCapabilities, HasSmallCapsFeatureAlwaysFalse) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);

  const FontHandle font = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));
  EXPECT_FALSE(backend.hasSmallCapsFeature(font));
}

TEST(TextBackendSimpleCapabilities, BitmapGlyphAlwaysNullopt) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);

  const FontHandle font = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));
  EXPECT_FALSE(backend.bitmapGlyph(font, 1, 1.0f).has_value());
}

// ── Full backend: advanced capabilities ─────────────────────────────────────

TEST(TextBackendFullCapabilities, DetectsCursiveScripts) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  EXPECT_TRUE(backend.isCursive(0x0627));   // Arabic alef
  EXPECT_FALSE(backend.isCursive('A'));      // Latin
  EXPECT_FALSE(backend.isCursive(0x4E00));  // CJK
}

TEST(TextBackendFullCapabilities, TinyFontAdvancesStayInScale) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple simpleBackend(fontManager, registry);
  TextBackendFull fullBackend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  constexpr std::string_view kText = "Some long text";
  const auto simpleShaped = simpleBackend.shapeRun(
      font, 0.24f, kText, 0, kText.size(), false, FontVariant::Normal, false);
  const auto fullShaped = fullBackend.shapeRun(
      font, 0.24f, kText, 0, kText.size(), false, FontVariant::Normal, false);

  ASSERT_EQ(simpleShaped.glyphs.size(), fullShaped.glyphs.size());

  double simpleAdvance = 0.0;
  for (const auto& glyph : simpleShaped.glyphs) {
    simpleAdvance += glyph.xAdvance;
  }

  double fullAdvance = 0.0;
  for (const auto& glyph : fullShaped.glyphs) {
    fullAdvance += glyph.xAdvance;
  }

  EXPECT_NEAR(fullAdvance, simpleAdvance, 0.5)
      << "simpleAdvance=" << simpleAdvance << ", fullAdvance=" << fullAdvance;
}

TEST(TextBackendFullCapabilities, DetectsSmallCapsFeature) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle notoSans = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(notoSans));
  EXPECT_TRUE(backend.hasSmallCapsFeature(notoSans));

  const FontHandle mplus = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(mplus));
  EXPECT_FALSE(backend.hasSmallCapsFeature(mplus));
}

TEST(TextBackendFullCapabilities, BitmapGlyphReturnsEmojiBitmap) {
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

}  // namespace

}  // namespace donner::svg
