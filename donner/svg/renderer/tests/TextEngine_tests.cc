#include "donner/svg/text/TextEngine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>

#include "donner/base/MathUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {

namespace {

components::ComputedTextComponent::TextSpan MakeSpan(const std::string& str) {
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString(str);
  span.start = 0;
  span.end = str.size();
  span.startsNewChunk = true;
  return span;
}

TextLayoutParams MakeTextParams(double fontSize) {
  TextLayoutParams params;
  params.fontSize = Lengthd(fontSize, Lengthd::Unit::Px);
  params.viewBox = Boxd(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  return params;
}

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

}  // namespace

TEST(TextEngineTest, UsesCoverageFallbackForArabicText) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
  const FontHandle amiri = LoadResvgFont(fontManager, "Amiri-Regular.ttf", "Amiri");
  ASSERT_TRUE(static_cast<bool>(amiri));

  components::ComputedTextComponent text;
  auto span = MakeSpan("\xD9\x87\xD8\xA7\xD9\x84\xD9\x88");  // هالو
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(110.0, Lengthd::Unit::None));
  span.rotateList = {45.0};
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(64.0);
  params.fontFamilies = {RcString("Noto Sans")};
  const auto runs = engine.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_FALSE(runs[0].glyphs.empty());
  EXPECT_EQ(runs[0].font, amiri);
  for (const auto& glyph : runs[0].glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
  }
}

TEST(TextEngineTest, UsesCoverageFallbackForVerticalJapaneseText) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "Vertical CJK offset assertions require the full text backend.";
#endif

  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  const FontHandle japaneseFont = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(japaneseFont));
  EXPECT_TRUE(static_cast<bool>(fontManager.findFont("Mplus 1p")));

  components::ComputedTextComponent text;
  auto span = MakeSpan("\xE6\x97\xA5\xE6\x9C\xAC");  // 日本
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(30.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(64.0);
  params.fontFamilies = {RcString("Mplus 1p")};
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  for (const auto& glyph : runs[0].glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0);
  }

  const float scale = engine.scaleForEmToPixels(runs[0].font, 64.0f);
  const PathSpline firstGlyphPath =
      engine.glyphOutline(runs[0].font, runs[0].glyphs[0].glyphIndex, scale);
  ASSERT_FALSE(firstGlyphPath.empty());

  Boxd positionedBounds = firstGlyphPath.bounds();
  positionedBounds += Vector2d(runs[0].glyphs[0].xPosition, runs[0].glyphs[0].yPosition);
  const double centerX = (positionedBounds.topLeft.x + positionedBounds.bottomRight.x) * 0.5;
  EXPECT_NEAR(centerX, 100.0, 2.0);
  EXPECT_GT(positionedBounds.topLeft.y, 34.0);
  EXPECT_LT(positionedBounds.topLeft.y, 42.0);
}

TEST(TextEngineTest, SupplementaryCharactersConsumeLowSurrogateCoordinates) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "Bitmap emoji shaping requires the full text backend.";
#endif

  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(
      static_cast<bool>(LoadResvgFont(fontManager, "NotoColorEmoji.ttf", "Noto Color Emoji")));

  components::ComputedTextComponent text;
  auto span = MakeSpan(
      "\xF0\x9F\x98\x81\xF0\x9F\xA6\x80\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D"
      "\xF0\x9F\x8C\x88");
  span.xList.push_back(Lengthd(52.0, Lengthd::Unit::None));
  span.yList = {Lengthd(90.0, Lengthd::Unit::None), Lengthd(111.0, Lengthd::Unit::None),
                Lengthd(130.0, Lengthd::Unit::None), Lengthd(150.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(32.0);
  params.fontFamilies = {RcString("Noto Color Emoji")};
  const auto runs = engine.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 3u);
  EXPECT_NEAR(runs[0].glyphs[0].yPosition, 90.0, 1.0);
  EXPECT_NEAR(runs[0].glyphs[1].yPosition, 130.0, 1.0);
  EXPECT_NEAR(runs[0].glyphs[2].yPosition, 150.0, 1.0);
}

TEST(TextEngineTest, RotatesCombiningMarkOffsetsWithBaseGlyph) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));

  components::ComputedTextComponent text;
  auto span = MakeSpan("o\xCC\xB2");
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  text.spans.push_back(span);

  TextLayoutParams params = MakeTextParams(64.0);
  params.fontFamilies = {RcString("Noto Sans")};

  const auto unrotatedRuns = engine.layout(text, params);
  ASSERT_EQ(unrotatedRuns.size(), 1u);
  ASSERT_EQ(unrotatedRuns[0].glyphs.size(), 2u);

  text.spans[0].rotateList = {30.0};
  const auto rotatedRuns = engine.layout(text, params);
  ASSERT_EQ(rotatedRuns.size(), 1u);
  ASSERT_EQ(rotatedRuns[0].glyphs.size(), 2u);

  const auto& unrotatedBase = unrotatedRuns[0].glyphs[0];
  const auto& unrotatedMark = unrotatedRuns[0].glyphs[1];
  const auto& rotatedBase = rotatedRuns[0].glyphs[0];
  const auto& rotatedMark = rotatedRuns[0].glyphs[1];

  const double dx = unrotatedMark.xPosition - unrotatedBase.xPosition;
  const double dy = unrotatedMark.yPosition - unrotatedBase.yPosition;
  const double angle = 30.0 * MathConstants<double>::kDegToRad;
  const double expectedDx = dx * std::cos(angle) - dy * std::sin(angle);
  const double expectedDy = dx * std::sin(angle) + dy * std::cos(angle);

  EXPECT_DOUBLE_EQ(rotatedBase.rotateDegrees, 30.0);
  EXPECT_DOUBLE_EQ(rotatedMark.rotateDegrees, 30.0);
  EXPECT_NEAR(rotatedMark.xPosition - rotatedBase.xPosition, expectedDx, 1.0);
  EXPECT_NEAR(rotatedMark.yPosition - rotatedBase.yPosition, expectedDy, 1.0);
}

}  // namespace donner::svg
