#include "donner/svg/renderer/TextShaper.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

namespace {

/// Build a simple ComputedTextComponent with one span.
components::ComputedTextComponent makeSimpleText(const std::string& str) {
  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString(str);
  span.start = 0;
  span.end = str.size();
  span.x = Lengthd(10.0, Lengthd::Unit::None);
  span.y = Lengthd(50.0, Lengthd::Unit::None);
  span.dx = Lengthd(0.0, Lengthd::Unit::None);
  span.dy = Lengthd(0.0, Lengthd::Unit::None);
  text.spans.push_back(std::move(span));
  return text;
}

/// Build TextParams with a given font size.
TextParams makeTextParams(double fontSize) {
  TextParams params;
  params.fontSize = Lengthd(fontSize, Lengthd::Unit::Px);
  params.viewBox = Boxd(Vector2d::Zero(), Vector2d(100, 100));
  params.fontMetrics = FontMetrics();
  return params;
}

}  // namespace

TEST(TextShaperTest, BasicShaping) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text = makeSimpleText("Hello");
  auto params = makeTextParams(16.0);

  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);

  const auto& run = runs[0];
  EXPECT_TRUE(static_cast<bool>(run.font));
  EXPECT_EQ(run.glyphs.size(), 5u);  // "Hello" = 5 characters

  // All glyphs should have valid glyph indices.
  for (const auto& glyph : run.glyphs) {
    EXPECT_GT(glyph.glyphIndex, 0) << "Glyph index should be non-zero for Latin characters";
  }

  // X positions should be monotonically increasing.
  for (size_t i = 1; i < run.glyphs.size(); ++i) {
    EXPECT_GT(run.glyphs[i].xPosition, run.glyphs[i - 1].xPosition)
        << "Glyph " << i << " should be to the right of glyph " << (i - 1);
  }

  // First glyph should start at the span's x position (10.0).
  EXPECT_DOUBLE_EQ(run.glyphs[0].xPosition, 10.0);

  // All glyphs should be at the same y baseline (50.0).
  for (const auto& glyph : run.glyphs) {
    EXPECT_DOUBLE_EQ(glyph.yPosition, 50.0);
  }
}

TEST(TextShaperTest, AdvanceWidthIsPositive) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text = makeSimpleText("W");
  auto params = makeTextParams(24.0);

  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);
  EXPECT_GT(runs[0].glyphs[0].xAdvance, 0.0);
}

TEST(TextShaperTest, LargerFontSizeProducesLargerAdvance) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text1 = makeSimpleText("A");
  auto params1 = makeTextParams(12.0);
  auto runs1 = shaper.layout(text1, params1);

  auto text2 = makeSimpleText("A");
  auto params2 = makeTextParams(24.0);
  auto runs2 = shaper.layout(text2, params2);

  ASSERT_EQ(runs1.size(), 1u);
  ASSERT_EQ(runs2.size(), 1u);
  ASSERT_EQ(runs1[0].glyphs.size(), 1u);
  ASSERT_EQ(runs2[0].glyphs.size(), 1u);

  EXPECT_GT(runs2[0].glyphs[0].xAdvance, runs1[0].glyphs[0].xAdvance);
}

TEST(TextShaperTest, TextAnchorMiddle) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text = makeSimpleText("Hello");
  auto params = makeTextParams(16.0);
  params.textAnchor = TextAnchor::Middle;

  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_GT(runs[0].glyphs.size(), 0u);

  // With text-anchor:middle, the first glyph should be to the left of x=10.
  EXPECT_LT(runs[0].glyphs[0].xPosition, 10.0);
}

TEST(TextShaperTest, EmptyText) {
  FontManager mgr;
  TextShaper shaper(mgr);

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString("");
  span.start = 0;
  span.end = 0;
  span.x = Lengthd(10.0, Lengthd::Unit::None);
  span.y = Lengthd(50.0, Lengthd::Unit::None);
  span.dx = Lengthd(0.0, Lengthd::Unit::None);
  span.dy = Lengthd(0.0, Lengthd::Unit::None);
  text.spans.push_back(std::move(span));

  auto params = makeTextParams(16.0);
  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_TRUE(runs[0].glyphs.empty());
}

TEST(TextShaperTest, Utf8Multibyte) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text = makeSimpleText("café");
  auto params = makeTextParams(16.0);

  std::vector<ShapedTextRun> runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  // "café" = 4 codepoints, HarfBuzz may produce ligatures but for Latin should be 4 glyphs.
  EXPECT_EQ(runs[0].glyphs.size(), 4u);
}

TEST(TextShaperTest, GlyphOutlineProducesPath) {
  FontManager mgr;
  TextShaper shaper(mgr);

  FontHandle font = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  // Get the glyph index for 'A' via shaping a single character.
  auto text = makeSimpleText("A");
  auto params = makeTextParams(16.0);
  auto runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);
  const int glyphIndex = runs[0].glyphs[0].glyphIndex;
  PathSpline spline = shaper.glyphOutline(font, glyphIndex, 0.02f);
  EXPECT_FALSE(spline.empty());
  EXPECT_GT(spline.commands().size(), 0u);
  EXPECT_GT(spline.points().size(), 0u);
}

}  // namespace donner::svg
