#include "donner/svg/renderer/TextShaper.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/renderer/TextLayout.h"

namespace donner::svg {

namespace {

/// Build a simple ComputedTextComponent with one span.
components::ComputedTextComponent makeSimpleText(const std::string& str) {
  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString(str);
  span.start = 0;
  span.end = str.size();
  span.xList.push_back(Lengthd(10.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  span.startsNewChunk = true;
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

TEST(TextShaperTest, SpanWithoutExplicitPositionContinuesFromPreviousSpan) {
  FontManager mgr;
  TextShaper shaper(mgr);

  components::ComputedTextComponent text;

  components::ComputedTextComponent::TextSpan span1;
  span1.text = RcString("AB");
  span1.start = 0;
  span1.end = 2;
  span1.xList.push_back(Lengthd(10.0, Lengthd::Unit::None));
  span1.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  span1.startsNewChunk = true;
  text.spans.push_back(std::move(span1));

  components::ComputedTextComponent::TextSpan span2;
  span2.text = RcString("CD");
  span2.start = 0;
  span2.end = 2;
  text.spans.push_back(std::move(span2));

  const auto runs = shaper.layout(text, makeTextParams(16.0));
  ASSERT_EQ(runs.size(), 2u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  ASSERT_EQ(runs[1].glyphs.size(), 2u);

  EXPECT_GT(runs[1].glyphs[0].xPosition, runs[0].glyphs.back().xPosition);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].yPosition, runs[0].glyphs.back().yPosition);
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
  span.xList.push_back(Lengthd(10.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  span.startsNewChunk = true;
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

TEST(TextShaperTest, GlyphOutlineBoundsMatchTextLayout) {
  FontManager mgr;
  TextLayout layout(mgr);
  TextShaper shaper(mgr);

  FontHandle font = mgr.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  auto text = makeSimpleText("T");
  auto params = makeTextParams(64.0);
  const auto layoutRuns = layout.layout(text, params);
  const auto shapedRuns = shaper.layout(text, params);

  ASSERT_EQ(layoutRuns.size(), 1u);
  ASSERT_EQ(shapedRuns.size(), 1u);
  ASSERT_EQ(layoutRuns[0].glyphs.size(), 1u);
  ASSERT_EQ(shapedRuns[0].glyphs.size(), 1u);

  const stbtt_fontinfo* info = mgr.fontInfo(font);
  ASSERT_NE(info, nullptr);

  const float scale = mgr.scaleForPixelHeight(font, 64.0f);
  const PathSpline layoutSpline =
      glyphToPathSpline(info, layoutRuns[0].glyphs[0].glyphIndex, scale);
  const PathSpline shapedSpline =
      shaper.glyphOutline(font, shapedRuns[0].glyphs[0].glyphIndex, scale);

  ASSERT_FALSE(layoutSpline.empty());
  ASSERT_FALSE(shapedSpline.empty());

  const Boxd layoutBounds = layoutSpline.bounds();
  const Boxd shapedBounds = shapedSpline.bounds();
  EXPECT_NEAR(shapedBounds.topLeft.x, layoutBounds.topLeft.x, 2.0);
  EXPECT_NEAR(shapedBounds.topLeft.y, layoutBounds.topLeft.y, 2.0);
  EXPECT_NEAR(shapedBounds.bottomRight.x, layoutBounds.bottomRight.x, 2.0);
  EXPECT_NEAR(shapedBounds.bottomRight.y, layoutBounds.bottomRight.y, 2.0);
}

TEST(TextShaperTest, GlyphOutlineClosesMultipleContours) {
  FontManager mgr;
  TextShaper shaper(mgr);

  auto text = makeSimpleText("e");
  auto params = makeTextParams(64.0);
  const auto runs = shaper.layout(text, params);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);

  FontHandle font = runs[0].font;
  ASSERT_TRUE(static_cast<bool>(font));

  const float scale = mgr.scaleForPixelHeight(font, 64.0f);
  const PathSpline spline = shaper.glyphOutline(font, runs[0].glyphs[0].glyphIndex, scale);
  ASSERT_FALSE(spline.empty());

  size_t moveCount = 0;
  size_t closeCount = 0;
  for (const auto& command : spline.commands()) {
    if (command.type == PathSpline::CommandType::MoveTo) {
      ++moveCount;
    } else if (command.type == PathSpline::CommandType::ClosePath) {
      ++closeCount;
    }
  }

  EXPECT_GE(moveCount, 2u);
  EXPECT_EQ(closeCount, moveCount);
}

/// Load a font from the resvg test suite's fonts directory and register it.
FontHandle loadResvgFont(FontManager& mgr, const std::string& fontFilename,
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

  mgr.addFontFace(face);
  return mgr.findFont(RcString(familyName));
}

// Test Arabic text shaping with per-character Y coordinates.
// Validates glyph IDs and positions for "الويب" with y="140 150 160 170"
// to determine whether chunk boundaries produce different joining forms.
TEST(TextShaperTest, ArabicPerCharacterY_GlyphIdsAndPositions) {
  FontManager mgr;
  TextShaper shaper(mgr);

  FontHandle amiri = loadResvgFont(mgr, "Amiri-Regular.ttf", "Amiri");
  ASSERT_TRUE(static_cast<bool>(amiri)) << "Could not load Amiri font";

  const std::string arabicText = "\xd8\xa7\xd9\x84\xd9\x88\xd9\x8a\xd8\xa8";  // الويب
  auto params = makeTextParams(48.0);
  params.fontFamilies = {RcString("Amiri")};

  // Case 1: No per-character coords (single chunk, all connected forms).
  auto textNoCoords = makeSimpleText(arabicText);
  textNoCoords.spans[0].xList[0] = Lengthd(50.0, Lengthd::Unit::None);
  textNoCoords.spans[0].yList[0] = Lengthd(60.0, Lengthd::Unit::None);

  const auto runsNoCoords = shaper.layout(textNoCoords, params);
  ASSERT_EQ(runsNoCoords.size(), 1u);
  ASSERT_EQ(runsNoCoords[0].glyphs.size(), 5u) << "Expected 5 glyphs for 5 Arabic characters";

  // Case 2: Per-character Y coords (multiple chunks).
  // y="140 150 160 170" → yList[0]=140, yList[1]=150, yList[2]=160, yList[3]=170
  components::ComputedTextComponent textWithCoords;
  {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString(arabicText);
    span.start = 0;
    span.end = arabicText.size();
    span.startsNewChunk = true;
    // xList: only index 0 has explicit position
    span.xList.resize(5);
    span.xList[0] = Lengthd(50.0, Lengthd::Unit::None);
    // yList: [140, 150, 160, 170, nullopt] (5 characters)
    span.yList.resize(5);
    span.yList[0] = Lengthd(140.0, Lengthd::Unit::None);
    span.yList[1] = Lengthd(150.0, Lengthd::Unit::None);
    span.yList[2] = Lengthd(160.0, Lengthd::Unit::None);
    span.yList[3] = Lengthd(170.0, Lengthd::Unit::None);
    span.yList[4] = std::nullopt;
    textWithCoords.spans.push_back(std::move(span));
  }

  const auto runsWithCoords = shaper.layout(textWithCoords, params);
  ASSERT_EQ(runsWithCoords.size(), 1u);
  ASSERT_EQ(runsWithCoords[0].glyphs.size(), 5u);

  // Print glyph details for both cases.
  std::cout << "\n=== Arabic per-char Y test ===\n";
  std::cout << "No per-char coords (single chunk):\n";
  for (size_t i = 0; i < runsNoCoords[0].glyphs.size(); ++i) {
    const auto& g = runsNoCoords[0].glyphs[i];
    std::cout << "  gi=" << i << " glyphId=" << g.glyphIndex << " x=" << g.xPosition
              << " y=" << g.yPosition << " xAdv=" << g.xAdvance << "\n";
  }

  std::cout << "\nWith per-char Y coords (chunks {ba}, {ya}, {waw}, {lam,alef}):\n";
  for (size_t i = 0; i < runsWithCoords[0].glyphs.size(); ++i) {
    const auto& g = runsWithCoords[0].glyphs[i];
    std::cout << "  gi=" << i << " glyphId=" << g.glyphIndex << " x=" << g.xPosition
              << " y=" << g.yPosition << " xAdv=" << g.xAdvance << "\n";
  }

  // Compare: do glyph IDs differ between the two cases?
  bool anyGlyphIdDiffers = false;
  for (size_t i = 0; i < 5; ++i) {
    if (runsNoCoords[0].glyphs[i].glyphIndex != runsWithCoords[0].glyphs[i].glyphIndex) {
      anyGlyphIdDiffers = true;
    }
  }
  std::cout << "\nGlyph IDs differ between cases: " << (anyGlyphIdDiffers ? "YES" : "NO") << "\n";

  // With per-char coords + RTL, layout switches to LTR (DOM order).
  // Glyphs are now: alef, lam, waw, ya, ba — each shaped individually.
  // All glyphs should differ from the no-coords case (different joining context).
  // Note: glyph order changed from visual RTL to DOM LTR, so indices don't
  // correspond to the same characters between the two runs.

  // Validate Y positions: with per-char coords on RTL text, layout switches to LTR
  // (DOM order). charIdx maps y values in DOM order:
  //   alef(charIdx=0)→y=140, lam(1)→150, waw(2)→160, ya(3)→170, ba(4)→inherits 170.
  // Glyphs are now in DOM order: alef, lam, waw, ya, ba.
  EXPECT_NEAR(runsWithCoords[0].glyphs[0].yPosition, 140.0, 1.0) << "alef at y=140 (yList[0])";
  EXPECT_NEAR(runsWithCoords[0].glyphs[1].yPosition, 150.0, 1.0) << "lam at y=150";
  EXPECT_NEAR(runsWithCoords[0].glyphs[2].yPosition, 160.0, 1.0) << "waw at y=160";
  EXPECT_NEAR(runsWithCoords[0].glyphs[3].yPosition, 170.0, 1.0) << "ya at y=170";
  EXPECT_NEAR(runsWithCoords[0].glyphs[4].yPosition, 170.0, 1.0) << "ba inherits y=170";

  // X positions should start at x=50 and increase monotonically (LTR DOM order).
  EXPECT_NEAR(runsWithCoords[0].glyphs[0].xPosition, 50.0, 1.0) << "First glyph (alef) at x=50";
  for (size_t i = 1; i < 5; ++i) {
    EXPECT_GT(runsWithCoords[0].glyphs[i].xPosition, runsWithCoords[0].glyphs[i - 1].xPosition)
        << "Glyph " << i << " should be to the right of glyph " << (i - 1);
  }
}

// Debug test: check glyph positions for e-tspan-027 pattern.
// <text x="33"><tspan y="100 110 120 130">T<tspan y="50">ex</tspan></tspan>t</text>
TEST(TextShaperTest, NestedTspanMultipleYCoordinates) {
  FontManager mgr;
  TextShaper shaper(mgr);

  components::ComputedTextComponent text;

  // Span 0: root empty (x=33, y=100 from tspan1 cascade)
  {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("");
    span.start = 0;
    span.end = 0;
    span.startsNewChunk = true;
    span.xList.push_back(Lengthd(33.0, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    text.spans.push_back(std::move(span));
  }
  // Span 1: tspan1 "T" (y=100)
  {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("T");
    span.start = 0;
    span.end = 1;
    span.startsNewChunk = true;
    span.xList.push_back(Lengthd(33.0, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    text.spans.push_back(std::move(span));
  }
  // Span 2: tspan2 "ex" (y=50, 120)
  {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("ex");
    span.start = 0;
    span.end = 2;
    span.startsNewChunk = true;
    span.xList.resize(2);  // no x values
    span.yList.resize(2);
    span.yList[0] = Lengthd(50.0, Lengthd::Unit::None);
    span.yList[1] = Lengthd(120.0, Lengthd::Unit::None);
    text.spans.push_back(std::move(span));
  }
  // Span 3: root continuation "t" (no explicit position, continues from "x")
  {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("t");
    span.start = 0;
    span.end = 1;
    span.startsNewChunk = false;
    span.xList.resize(1);
    span.yList.resize(1);
    text.spans.push_back(std::move(span));
  }

  auto params = makeTextParams(64.0);
  auto runs = shaper.layout(text, params);

  // Print positions for debugging.
  const char* labels[] = {"(empty)", "T", "e", "x", "t"};
  int li = 0;
  for (size_t ri = 0; ri < runs.size(); ++ri) {
    for (const auto& g : runs[ri].glyphs) {
      std::cout << "  " << labels[li + 1] << ": x=" << g.xPosition << " y=" << g.yPosition
                << " xAdv=" << g.xAdvance << "\n";
      ++li;
    }
  }

  // Basic checks: T at x=33, y=100. e at y=50. x at y=120.
  ASSERT_EQ(runs.size(), 4u);
  ASSERT_EQ(runs[1].glyphs.size(), 1u);  // T
  ASSERT_EQ(runs[2].glyphs.size(), 2u);  // ex
  ASSERT_EQ(runs[3].glyphs.size(), 1u);  // t

  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].xPosition, 33.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].yPosition, 100.0);
  EXPECT_DOUBLE_EQ(runs[2].glyphs[0].yPosition, 50.0);   // e
  EXPECT_DOUBLE_EQ(runs[2].glyphs[1].yPosition, 120.0);  // x

  // x positions: e should continue from T, x should continue from e.
  double afterT = runs[1].glyphs[0].xPosition + runs[1].glyphs[0].xAdvance;
  EXPECT_NEAR(runs[2].glyphs[0].xPosition, afterT, 1.0) << "e should continue from T";

  double afterE = runs[2].glyphs[0].xPosition + runs[2].glyphs[0].xAdvance;
  EXPECT_NEAR(runs[2].glyphs[1].xPosition, afterE, 1.0) << "x should continue from e";

  double afterX = runs[2].glyphs[1].xPosition + runs[2].glyphs[1].xAdvance;
  EXPECT_NEAR(runs[3].glyphs[0].xPosition, afterX, 2.0) << "t should continue from x";
}

}  // namespace donner::svg
