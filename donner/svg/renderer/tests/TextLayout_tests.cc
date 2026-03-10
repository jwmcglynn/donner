#include "donner/svg/renderer/TextLayout.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

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

TEST(TextLayoutTest, BasicLayout) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("Hello");
  auto params = makeTextParams(16.0);

  std::vector<LayoutTextRun> runs = layout.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);

  const auto& run = runs[0];
  EXPECT_TRUE(static_cast<bool>(run.font));
  EXPECT_EQ(run.glyphs.size(), 5u);  // "Hello" = 5 characters

  // All glyphs should have valid glyph indices (Public Sans has Latin glyphs).
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

TEST(TextLayoutTest, MultipleSpans) {
  FontManager mgr;
  TextLayout layout(mgr);

  components::ComputedTextComponent text;

  // First span.
  components::ComputedTextComponent::TextSpan span1;
  span1.text = RcString("AB");
  span1.start = 0;
  span1.end = 2;
  span1.x = Lengthd(10.0, Lengthd::Unit::None);
  span1.y = Lengthd(50.0, Lengthd::Unit::None);
  span1.dx = Lengthd(0.0, Lengthd::Unit::None);
  span1.dy = Lengthd(0.0, Lengthd::Unit::None);
  text.spans.push_back(std::move(span1));

  // Second span at a different position.
  components::ComputedTextComponent::TextSpan span2;
  span2.text = RcString("CD");
  span2.start = 0;
  span2.end = 2;
  span2.x = Lengthd(80.0, Lengthd::Unit::None);
  span2.y = Lengthd(60.0, Lengthd::Unit::None);
  span2.dx = Lengthd(0.0, Lengthd::Unit::None);
  span2.dy = Lengthd(0.0, Lengthd::Unit::None);
  text.spans.push_back(std::move(span2));

  auto params = makeTextParams(16.0);
  std::vector<LayoutTextRun> runs = layout.layout(text, params);

  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_EQ(runs[1].glyphs.size(), 2u);

  // First span starts at x=10, second at x=80.
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 10.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].xPosition, 80.0);

  // Different y baselines.
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 50.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].yPosition, 60.0);
}

TEST(TextLayoutTest, DxDyOffset) {
  FontManager mgr;
  TextLayout layout(mgr);

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString("A");
  span.start = 0;
  span.end = 1;
  span.x = Lengthd(10.0, Lengthd::Unit::None);
  span.y = Lengthd(50.0, Lengthd::Unit::None);
  span.dx = Lengthd(5.0, Lengthd::Unit::None);
  span.dy = Lengthd(3.0, Lengthd::Unit::None);
  text.spans.push_back(std::move(span));

  auto params = makeTextParams(16.0);
  std::vector<LayoutTextRun> runs = layout.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);

  // Position should include dx/dy offsets.
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 15.0);  // 10 + 5
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 53.0);  // 50 + 3
}

TEST(TextLayoutTest, Rotation) {
  FontManager mgr;
  TextLayout layout(mgr);

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString("A");
  span.start = 0;
  span.end = 1;
  span.x = Lengthd(10.0, Lengthd::Unit::None);
  span.y = Lengthd(50.0, Lengthd::Unit::None);
  span.dx = Lengthd(0.0, Lengthd::Unit::None);
  span.dy = Lengthd(0.0, Lengthd::Unit::None);
  span.rotateDegrees = 45.0;
  text.spans.push_back(std::move(span));

  auto params = makeTextParams(16.0);
  std::vector<LayoutTextRun> runs = layout.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].rotateDegrees, 45.0);
}

TEST(TextLayoutTest, EmptyText) {
  FontManager mgr;
  TextLayout layout(mgr);

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
  std::vector<LayoutTextRun> runs = layout.layout(text, params);

  ASSERT_EQ(runs.size(), 1u);
  EXPECT_TRUE(runs[0].glyphs.empty());
}

TEST(TextLayoutTest, AdvanceWidthIsPositive) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("W");
  auto params = makeTextParams(24.0);

  std::vector<LayoutTextRun> runs = layout.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);

  // 'W' should have a positive advance width.
  EXPECT_GT(runs[0].glyphs[0].xAdvance, 0.0);
}

TEST(TextLayoutTest, LargerFontSizeProducesLargerAdvance) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text1 = makeSimpleText("A");
  auto params1 = makeTextParams(12.0);
  auto runs1 = layout.layout(text1, params1);

  auto text2 = makeSimpleText("A");
  auto params2 = makeTextParams(24.0);
  auto runs2 = layout.layout(text2, params2);

  ASSERT_EQ(runs1.size(), 1u);
  ASSERT_EQ(runs2.size(), 1u);
  ASSERT_EQ(runs1[0].glyphs.size(), 1u);
  ASSERT_EQ(runs2[0].glyphs.size(), 1u);

  EXPECT_GT(runs2[0].glyphs[0].xAdvance, runs1[0].glyphs[0].xAdvance);
}

TEST(TextLayoutTest, Utf8Multibyte) {
  FontManager mgr;
  TextLayout layout(mgr);

  // Test with a multi-byte UTF-8 string (é = 2 bytes).
  auto text = makeSimpleText("café");
  auto params = makeTextParams(16.0);

  std::vector<LayoutTextRun> runs = layout.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  // "café" = 4 codepoints (c, a, f, é), even though it's 5 bytes.
  EXPECT_EQ(runs[0].glyphs.size(), 4u);
}

TEST(TextLayoutTest, TextAnchorMiddle) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("Hello");
  auto params = makeTextParams(16.0);
  params.textAnchor = TextAnchor::Middle;

  std::vector<LayoutTextRun> runs = layout.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_GT(runs[0].glyphs.size(), 0u);

  // With text-anchor:middle, the text should be centered on the anchor point (x=10).
  // The first glyph should be to the left of x=10.
  EXPECT_LT(runs[0].glyphs[0].xPosition, 10.0);
}

TEST(TextLayoutTest, TextAnchorEnd) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("Hello");
  auto params = makeTextParams(16.0);
  params.textAnchor = TextAnchor::End;

  std::vector<LayoutTextRun> runs = layout.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_GT(runs[0].glyphs.size(), 0u);

  // With text-anchor:end, the text should end at x=10.
  // The first glyph should be well to the left of x=10.
  EXPECT_LT(runs[0].glyphs[0].xPosition, 0.0);

  // The last glyph position + advance should be approximately at x=10.
  const auto& lastGlyph = runs[0].glyphs.back();
  EXPECT_NEAR(lastGlyph.xPosition + lastGlyph.xAdvance, 10.0, 0.5);
}

TEST(TextLayoutTest, TextLengthSpacing) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("AB");
  auto paramsNatural = makeTextParams(16.0);
  auto runsNatural = layout.layout(text, paramsNatural);
  ASSERT_EQ(runsNatural.size(), 1u);
  ASSERT_EQ(runsNatural[0].glyphs.size(), 2u);
  const double naturalWidth = runsNatural[0].glyphs[1].xPosition +
                               runsNatural[0].glyphs[1].xAdvance -
                               runsNatural[0].glyphs[0].xPosition;

  // Set textLength to double the natural width.
  auto text2 = makeSimpleText("AB");
  auto paramsStretched = makeTextParams(16.0);
  paramsStretched.textLength = Lengthd(naturalWidth * 2 + 10.0, Lengthd::Unit::None);
  paramsStretched.lengthAdjust = LengthAdjust::Spacing;

  auto runsStretched = layout.layout(text2, paramsStretched);
  ASSERT_EQ(runsStretched.size(), 1u);
  ASSERT_EQ(runsStretched[0].glyphs.size(), 2u);

  // The second glyph should be further from the first than in the natural layout.
  const double naturalGap =
      runsNatural[0].glyphs[1].xPosition - runsNatural[0].glyphs[0].xPosition;
  const double stretchedGap =
      runsStretched[0].glyphs[1].xPosition - runsStretched[0].glyphs[0].xPosition;
  EXPECT_GT(stretchedGap, naturalGap);
}

TEST(TextLayoutTest, DominantBaselineHanging) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("A");
  auto paramsDefault = makeTextParams(16.0);
  auto runsDefault = layout.layout(text, paramsDefault);
  ASSERT_EQ(runsDefault.size(), 1u);
  ASSERT_EQ(runsDefault[0].glyphs.size(), 1u);

  auto text2 = makeSimpleText("A");
  auto paramsHanging = makeTextParams(16.0);
  paramsHanging.dominantBaseline = DominantBaseline::Hanging;
  auto runsHanging = layout.layout(text2, paramsHanging);
  ASSERT_EQ(runsHanging.size(), 1u);
  ASSERT_EQ(runsHanging[0].glyphs.size(), 1u);

  // With dominant-baseline:hanging, the alphabetic baseline shifts down (larger y),
  // so the glyph y-position should be greater than the default.
  EXPECT_GT(runsHanging[0].glyphs[0].yPosition, runsDefault[0].glyphs[0].yPosition);
}

TEST(TextLayoutTest, DominantBaselineTextBottom) {
  FontManager mgr;
  TextLayout layout(mgr);

  auto text = makeSimpleText("A");
  auto paramsDefault = makeTextParams(16.0);
  auto runsDefault = layout.layout(text, paramsDefault);
  ASSERT_EQ(runsDefault.size(), 1u);
  ASSERT_EQ(runsDefault[0].glyphs.size(), 1u);

  auto text2 = makeSimpleText("A");
  auto paramsTextBottom = makeTextParams(16.0);
  paramsTextBottom.dominantBaseline = DominantBaseline::TextBottom;
  auto runsTextBottom = layout.layout(text2, paramsTextBottom);
  ASSERT_EQ(runsTextBottom.size(), 1u);
  ASSERT_EQ(runsTextBottom[0].glyphs.size(), 1u);

  // With dominant-baseline:text-bottom, the alphabetic baseline shifts up (smaller y),
  // so the glyph y-position should be less than the default.
  EXPECT_LT(runsTextBottom[0].glyphs[0].yPosition, runsDefault[0].glyphs[0].yPosition);
}

// Test glyph outline extraction.
TEST(GlyphToPathSplineTest, BasicOutline) {
  FontManager mgr;
  FontHandle font = mgr.fallbackFont();
  const stbtt_fontinfo* info = mgr.fontInfo(font);
  ASSERT_NE(info, nullptr);

  int glyphIndex = stbtt_FindGlyphIndex(info, 'A');
  ASSERT_GT(glyphIndex, 0);

  float scale = mgr.scaleForPixelHeight(font, 16.0f);
  PathSpline spline = glyphToPathSpline(info, glyphIndex, scale);
  EXPECT_FALSE(spline.empty());
  EXPECT_GT(spline.commands().size(), 0u);
  EXPECT_GT(spline.points().size(), 0u);
}

TEST(GlyphToPathSplineTest, SpaceGlyphMayBeEmpty) {
  FontManager mgr;
  FontHandle font = mgr.fallbackFont();
  const stbtt_fontinfo* info = mgr.fontInfo(font);
  ASSERT_NE(info, nullptr);

  int glyphIndex = stbtt_FindGlyphIndex(info, ' ');
  ASSERT_GT(glyphIndex, 0);

  float scale = mgr.scaleForPixelHeight(font, 16.0f);
  // Space typically has no outline — should not crash.
  PathSpline spline = glyphToPathSpline(info, glyphIndex, scale);
  // Just verify no crash; space usually has empty path.
}

TEST(GlyphToPathSplineTest, NotdefGlyphIndex) {
  FontManager mgr;
  FontHandle font = mgr.fallbackFont();
  const stbtt_fontinfo* info = mgr.fontInfo(font);
  ASSERT_NE(info, nullptr);

  // Glyph index 0 is .notdef — it should produce an outline without crashing.
  float scale = mgr.scaleForPixelHeight(font, 16.0f);
  PathSpline spline = glyphToPathSpline(info, 0, scale);
  // .notdef may or may not have an outline, just verify no crash.
}

}  // namespace donner::svg
