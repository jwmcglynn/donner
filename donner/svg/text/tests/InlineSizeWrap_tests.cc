/**
 * Tests for the SVG2 `inline-size` auto-flow wrapping helper (text_engine_detail).
 *
 * These are font-independent: glyph advances and positions are synthesized directly so the wrap
 * algorithm (greedy line filling, overflow words, trailing-space hanging, per-line text-anchor)
 * can be exercised deterministically.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/text/TextEngineHelpers.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg::text_engine_detail {
namespace {

using components::ComputedTextComponent;
using testing::DoubleNear;

constexpr double kAdvance = 10.0;  // Every synthetic glyph advances 10px.
constexpr double kBaseline = 20.0;
constexpr double kEps = 1e-6;

/// Build a single-span ComputedTextComponent plus a parallel TextRun where each byte of \p content
/// becomes one glyph with advance 10 laid out left to right starting at \p originX. This mirrors
/// the flat single-line layout that `applyInlineSizeWrap` consumes.
struct Synth {
  ComputedTextComponent text;
  std::vector<TextRun> runs;
};

Synth makeSingleSpan(std::string_view content, double originX = 0.0) {
  Synth s;
  ComputedTextComponent::TextSpan span;
  span.text = RcString(content);
  span.start = 0;
  span.end = content.size();
  s.text.spans.push_back(span);

  TextRun run;
  for (size_t i = 0; i < content.size(); ++i) {
    TextGlyph g;
    g.glyphIndex = static_cast<int>(i) + 1;
    g.xPosition = originX + static_cast<double>(i) * kAdvance;
    g.yPosition = kBaseline;
    g.xAdvance = kAdvance;
    g.cluster = static_cast<uint32_t>(i);
    run.glyphs.push_back(g);
  }
  s.runs.push_back(std::move(run));
  return s;
}

TextLayoutParams makeParams(double measurePx, TextAnchor anchor = TextAnchor::Start) {
  TextLayoutParams params;
  params.inlineSizePx = measurePx;
  params.textAnchor = anchor;
  return params;
}

// Convenience accessor for the (single-run) glyphs after wrapping.
const std::vector<TextGlyph>& glyphs(const Synth& s) { return s.runs[0].glyphs; }

TEST(InlineSizeWrap, FitsOnOneLineReturnsFalse) {
  Synth s = makeSingleSpan("aaa bbb");  // width 70
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(1000.0), 1000.0, 24.0);
  EXPECT_FALSE(wrapped);
  // Positions untouched.
  EXPECT_THAT(glyphs(s)[0].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(glyphs(s)[4].xPosition, DoubleNear(40.0, kEps));
  EXPECT_THAT(glyphs(s)[4].yPosition, DoubleNear(kBaseline, kEps));
}

TEST(InlineSizeWrap, GreedyWrapAtSpaces) {
  // "aaa bbb ccc": each word is 30 wide; measure 35 forces one word per line.
  Synth s = makeSingleSpan("aaa bbb ccc");
  const double lineHeight = 24.0;
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(35.0), 35.0, lineHeight);
  ASSERT_TRUE(wrapped);

  const auto& g = glyphs(s);
  // Line 0: "aaa" at x 0,10,20 on the base baseline.
  EXPECT_THAT(g[0].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[2].xPosition, DoubleNear(20.0, kEps));
  EXPECT_THAT(g[0].yPosition, DoubleNear(kBaseline, kEps));
  // Line 1: "bbb" reset to origin, one line down.
  EXPECT_THAT(g[4].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[6].xPosition, DoubleNear(20.0, kEps));
  EXPECT_THAT(g[4].yPosition, DoubleNear(kBaseline + lineHeight, kEps));
  // Line 2: "ccc" two lines down.
  EXPECT_THAT(g[8].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[10].xPosition, DoubleNear(20.0, kEps));
  EXPECT_THAT(g[10].yPosition, DoubleNear(kBaseline + 2.0 * lineHeight, kEps));
}

TEST(InlineSizeWrap, TwoWordsPerLine) {
  // measure 65 fits "aaa bbb" (70?) -> "aaa"+space+"bbb" = 30+10+30 = 70 > 65, so one word/line.
  // Use measure 75 to fit two words: "aaa bbb" = 70 <= 75, then "ccc" wraps.
  Synth s = makeSingleSpan("aaa bbb ccc");
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(75.0), 75.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  // Line 0: "aaa bbb" -> a at 0, b's continue after the space (space width preserved).
  EXPECT_THAT(g[0].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[4].xPosition, DoubleNear(40.0, kEps));  // first 'b' keeps its gap after the space
  EXPECT_THAT(g[4].yPosition, DoubleNear(kBaseline, kEps));
  // Line 1: "ccc" wrapped to origin.
  EXPECT_THAT(g[8].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[8].yPosition, DoubleNear(kBaseline + 24.0, kEps));
}

TEST(InlineSizeWrap, OverflowWordPlacedAlone) {
  // A word longer than the measure is never broken: it is placed alone and allowed to overflow.
  Synth s = makeSingleSpan("hi bbbbbbbbbb");  // "hi"=20, second word=100
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(30.0), 30.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  // Line 0: "hi".
  EXPECT_THAT(g[0].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[1].xPosition, DoubleNear(10.0, kEps));
  // Line 1: the long word starts at origin and overflows to the right.
  EXPECT_THAT(g[3].xPosition, DoubleNear(0.0, kEps));    // first char of long word at origin
  EXPECT_THAT(g[12].xPosition, DoubleNear(90.0, kEps));  // last char overflows past the measure
  EXPECT_THAT(g[3].yPosition, DoubleNear(kBaseline + 24.0, kEps));
}

TEST(InlineSizeWrap, TrailingSpacesHangAndDoNotCountToWidth) {
  // "aaa   bbb": three spaces between the words. With measure 35, "bbb" wraps and the three
  // trailing spaces hang on line 0 (not carried to line 1).
  Synth s = makeSingleSpan("aaa   bbb");
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(35.0), 35.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  // Line 0 "aaa" at origin.
  EXPECT_THAT(g[0].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[2].xPosition, DoubleNear(20.0, kEps));
  EXPECT_THAT(g[2].yPosition, DoubleNear(kBaseline, kEps));
  // The three spaces (indices 3,4,5) stay on line 0 (same baseline as the 'a's).
  EXPECT_THAT(g[3].yPosition, DoubleNear(kBaseline, kEps));
  EXPECT_THAT(g[5].yPosition, DoubleNear(kBaseline, kEps));
  // Line 1: "bbb" (indices 6,7,8) starts at origin, one line down.
  EXPECT_THAT(g[6].xPosition, DoubleNear(0.0, kEps));
  EXPECT_THAT(g[8].xPosition, DoubleNear(20.0, kEps));
  EXPECT_THAT(g[6].yPosition, DoubleNear(kBaseline + 24.0, kEps));
}

TEST(InlineSizeWrap, PerLineTextAnchorMiddle) {
  // Each wrapped line is independently centered about the block origin (x = 0).
  Synth s = makeSingleSpan("aaa bbbbb");  // "aaa"=30, "bbbbb"=50
  const bool wrapped =
      applyInlineSizeWrap(s.runs, s.text, makeParams(35.0, TextAnchor::Middle), 35.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  // Line 0 "aaa" used width 30 -> shift -15: glyphs at -15,-5,5.
  EXPECT_THAT(g[0].xPosition, DoubleNear(-15.0, kEps));
  EXPECT_THAT(g[2].xPosition, DoubleNear(5.0, kEps));
  // Line 1 "bbbbb" used width 50 -> shift -25: first glyph at -25.
  EXPECT_THAT(g[4].xPosition, DoubleNear(-25.0, kEps));
  EXPECT_THAT(g[8].xPosition, DoubleNear(15.0, kEps));
}

TEST(InlineSizeWrap, PerLineTextAnchorEnd) {
  // End anchor: each line's right edge sits at the block origin (x = 0).
  Synth s = makeSingleSpan("aaa bbbbb");
  const bool wrapped =
      applyInlineSizeWrap(s.runs, s.text, makeParams(35.0, TextAnchor::End), 35.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  // Line 0 "aaa" width 30 -> shift -30: glyphs at -30,-20,-10 (right edge of last = 0).
  EXPECT_THAT(g[0].xPosition, DoubleNear(-30.0, kEps));
  EXPECT_THAT(g[2].xPosition + g[2].xAdvance, DoubleNear(0.0, kEps));
  // Line 1 "bbbbb" width 50 -> right edge 0.
  EXPECT_THAT(g[8].xPosition + g[8].xAdvance, DoubleNear(0.0, kEps));
}

TEST(InlineSizeWrap, NonZeroOriginPreserved) {
  // The block origin (the x attribute) anchors line starts; wrapped lines return to it.
  Synth s = makeSingleSpan("aaa bbb", /*originX=*/100.0);
  const bool wrapped = applyInlineSizeWrap(s.runs, s.text, makeParams(35.0), 35.0, 24.0);
  ASSERT_TRUE(wrapped);
  const auto& g = glyphs(s);
  EXPECT_THAT(g[0].xPosition, DoubleNear(100.0, kEps));  // line 0 at origin
  EXPECT_THAT(g[4].xPosition, DoubleNear(100.0, kEps));  // line 1 back to origin
  EXPECT_THAT(g[4].yPosition, DoubleNear(kBaseline + 24.0, kEps));
}

TEST(InlineSizeWrap, ZeroMeasureIsNoOp) {
  Synth s = makeSingleSpan("aaa bbb ccc");
  EXPECT_FALSE(applyInlineSizeWrap(s.runs, s.text, makeParams(0.0), 0.0, 24.0));
  EXPECT_THAT(glyphs(s)[10].xPosition, DoubleNear(100.0, kEps));  // unchanged
}

}  // namespace
}  // namespace donner::svg::text_engine_detail
