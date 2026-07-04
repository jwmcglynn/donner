#include "donner/svg/text/TextEngineHelpers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/MockTextBackend.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {

using text_engine_detail::applyTextAnchor;
using text_engine_detail::applyTextLength;
using text_engine_detail::buildByteIndexMappings;
using text_engine_detail::ByteIndexMappings;
using text_engine_detail::ChunkBoundary;
using text_engine_detail::computeBaselineShift;
using text_engine_detail::computeSpanBaselineShiftPx;
using text_engine_detail::findChunkRanges;
using text_engine_detail::RunPenExtent;

namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FloatEq;
using ::testing::IsEmpty;
using ::testing::SizeIs;

auto ChunkRangeIs(size_t byteStart, size_t byteEnd) {
  return AllOf(Field("byteStart", &text_engine_detail::ChunkRange::byteStart, byteStart),
               Field("byteEnd", &text_engine_detail::ChunkRange::byteEnd, byteEnd));
}

auto GlyphXPositionIs(auto matcher) {
  return Field("xPosition", &TextGlyph::xPosition, matcher);
}

auto GlyphYPositionIs(auto matcher) {
  return Field("yPosition", &TextGlyph::yPosition, matcher);
}

auto GlyphXAdvanceIs(auto matcher) {
  return Field("xAdvance", &TextGlyph::xAdvance, matcher);
}

auto GlyphYAdvanceIs(auto matcher) {
  return Field("yAdvance", &TextGlyph::yAdvance, matcher);
}

auto GlyphIndexIs(auto matcher) {
  return Field("glyphIndex", &TextGlyph::glyphIndex, matcher);
}

auto GlyphRotateDegreesIs(auto matcher) {
  return Field("rotateDegrees", &TextGlyph::rotateDegrees, matcher);
}

auto GlyphFontSizeScaleIs(auto matcher) {
  return Field("fontSizeScale", &TextGlyph::fontSizeScale, matcher);
}

auto GlyphStretchScaleXIs(auto matcher) {
  return Field("stretchScaleX", &TextGlyph::stretchScaleX, matcher);
}

auto GlyphStretchScaleYIs(auto matcher) {
  return Field("stretchScaleY", &TextGlyph::stretchScaleY, matcher);
}

auto RunGlyphsAre(auto matcher) {
  return Field("glyphs", &TextRun::glyphs, matcher);
}

// ── computeBaselineShift ────────────────────────────────────────────────────

TEST(ComputeBaselineShiftTest, AutoReturnsZero) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Auto, vm, 1.0f), 0.0);
}

TEST(ComputeBaselineShiftTest, AlphabeticReturnsZero) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Alphabetic, vm, 1.0f), 0.0);
}

TEST(ComputeBaselineShiftTest, DeprecatedSvg11KeywordsReturnZero) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::UseScript, vm, 1.0f), 0.0);
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::NoChange, vm, 1.0f), 0.0);
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::ResetSize, vm, 1.0f), 0.0);
}

TEST(ComputeBaselineShiftTest, MiddleIsHalfXHeight) {
  FontVMetrics vm{800, -200, 0, /*xHeight=*/500};
  // 500 * 0.5 * 1.0 = 250.0
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Middle, vm, 1.0f), 250.0);
}

TEST(ComputeBaselineShiftTest, MiddleFallsBackTo45PercentHeightWithoutXHeight) {
  FontVMetrics vm{800, -200, 0};  // xHeight = 0 (font has no OS/2 sxHeight).
  // 0.45 * (800 - (-200)) * 0.5 * 1.0 = 225.0
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Middle, vm, 1.0f), 225.0);
}

TEST(ComputeBaselineShiftTest, CentralCentersEmBox) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Central, vm, 1.0f), 300.0);
}

TEST(ComputeBaselineShiftTest, HangingIsEightyPercentAscent) {
  FontVMetrics vm{1000, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Hanging, vm, 1.0f), 800.0);
}

TEST(ComputeBaselineShiftTest, MathematicalIsHalfAscent) {
  FontVMetrics vm{1000, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Mathematical, vm, 1.0f), 500.0);
}

TEST(ComputeBaselineShiftTest, TextTopIsFullAscent) {
  FontVMetrics vm{1000, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::TextTop, vm, 1.0f), 1000.0);
}

TEST(ComputeBaselineShiftTest, TextBottomIsDescent) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::TextBottom, vm, 1.0f), -200.0);
}

TEST(ComputeBaselineShiftTest, IdeographicIsDescent) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Ideographic, vm, 1.0f), -200.0);
}

TEST(ComputeBaselineShiftTest, ScaleIsApplied) {
  FontVMetrics vm{1000, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Hanging, vm, 0.5f), 400.0);
}

// ── findChunkRanges ─────────────────────────────────────────────────────────

TEST(FindChunkRangesTest, SingleChunkWhenNoAbsolutePositions) {
  SmallVector<std::optional<Lengthd>, 1> xList;
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("Hello", xList, yList);
  EXPECT_THAT(ranges, ElementsAre(ChunkRangeIs(0u, 5u)));
}

TEST(FindChunkRangesTest, SplitsAtAbsoluteXPosition) {
  SmallVector<std::optional<Lengthd>, 1> xList = {std::nullopt, std::nullopt,
                                                  Lengthd(50.0, Lengthd::Unit::None)};
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("ABC", xList, yList);
  EXPECT_THAT(ranges, ElementsAre(ChunkRangeIs(0u, 2u), ChunkRangeIs(2u, 3u)));
}

TEST(FindChunkRangesTest, SplitsAtAbsoluteYPosition) {
  SmallVector<std::optional<Lengthd>, 1> xList;
  SmallVector<std::optional<Lengthd>, 1> yList = {std::nullopt,
                                                  Lengthd(100.0, Lengthd::Unit::None)};
  const auto ranges = findChunkRanges("AB", xList, yList);
  EXPECT_THAT(ranges, ElementsAre(ChunkRangeIs(0u, 1u), ChunkRangeIs(1u, 2u)));
}

TEST(FindChunkRangesTest, HandlesMultibyteUtf8) {
  // "Aé" = 'A'(1 byte) + 'é'(2 bytes: 0xC3 0xA9)
  SmallVector<std::optional<Lengthd>, 1> xList = {std::nullopt, Lengthd(50.0, Lengthd::Unit::None)};
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("A\xC3\xA9", xList, yList);
  EXPECT_THAT(ranges, ElementsAre(ChunkRangeIs(0u, 1u), ChunkRangeIs(1u, 3u)));
}

TEST(FindChunkRangesTest, KeepsJoinerAndVariationSelectorWithBaseCluster) {
  // A + ZWJ + B + VS16 + C.  The joiner sequence and variation selector stay in the
  // first SVG addressable character cluster; the absolute x at character index 1 starts C.
  const std::string text =
      "A\xE2\x80\x8D"
      "B\xEF\xB8\x8F"
      "C";
  SmallVector<std::optional<Lengthd>, 1> xList = {std::nullopt, Lengthd(50.0, Lengthd::Unit::None)};
  SmallVector<std::optional<Lengthd>, 1> yList;

  const auto ranges = findChunkRanges(text, xList, yList);

  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 8u);
  EXPECT_EQ(ranges[1].byteStart, 8u);
  EXPECT_EQ(ranges[1].byteEnd, 9u);
}

TEST(FindChunkRangesTest, EmptyTextReturnsSingleEmptyRange) {
  SmallVector<std::optional<Lengthd>, 1> xList;
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("", xList, yList);
  EXPECT_THAT(ranges, ElementsAre(ChunkRangeIs(0u, 0u)));
}

// ── buildByteIndexMappings ──────────────────────────────────────────────────

TEST(BuildByteIndexMappingsTest, AsciiText) {
  const auto m = buildByteIndexMappings("ABC");
  EXPECT_THAT(m.byteToCharIdx, ElementsAre(0u, 1u, 2u));
  EXPECT_THAT(m.byteToRawCpIdx, ElementsAre(0u, 1u, 2u));
}

TEST(BuildByteIndexMappingsTest, CombiningMarkSharesBaseIndex) {
  // "o" + combining low line (U+0332, 2 bytes: CC B2)
  const auto m = buildByteIndexMappings("o\xCC\xB2");
  EXPECT_THAT(m.byteToCharIdx, ElementsAre(0u, 0u, 0u));
}

TEST(BuildByteIndexMappingsTest, JoinerAndVariationSelectorShareBaseIndex) {
  const auto m = buildByteIndexMappings(
      "A\xE2\x80\x8D"
      "B\xEF\xB8\x8F"
      "C");

  ASSERT_EQ(m.byteToCharIdx.size(), 9u);
  EXPECT_EQ(m.byteToCharIdx[0], 0u);  // A.
  EXPECT_EQ(m.byteToCharIdx[1], 0u);  // ZWJ.
  EXPECT_EQ(m.byteToCharIdx[4], 0u);  // B following ZWJ.
  EXPECT_EQ(m.byteToCharIdx[5], 0u);  // VS16.
  EXPECT_EQ(m.byteToCharIdx[8], 1u);  // C starts the next addressable character.

  EXPECT_EQ(m.byteToRawCpIdx[0], 0u);
  EXPECT_EQ(m.byteToRawCpIdx[1], 1u);
  EXPECT_EQ(m.byteToRawCpIdx[4], 2u);
  EXPECT_EQ(m.byteToRawCpIdx[5], 3u);
  EXPECT_EQ(m.byteToRawCpIdx[8], 4u);
}

TEST(BuildByteIndexMappingsTest, SupplementaryCharacterConsumeTwoIndices) {
  // "A" + U+1F601 (😁, 4 bytes) + "B". Supplementary chars increment charIdx by 2
  // (UTF-16 surrogate pair semantics) when they're not the first character.
  const auto m = buildByteIndexMappings(
      "A\xF0\x9F\x98\x81"
      "B");
  EXPECT_THAT(m.byteToCharIdx, ElementsAre(0u, 2u, 2u, 2u, 2u, 3u));
}

TEST(BuildByteIndexMappingsTest, EmptyText) {
  const auto m = buildByteIndexMappings("");
  EXPECT_THAT(m.byteToCharIdx, IsEmpty());
  EXPECT_THAT(m.byteToRawCpIdx, IsEmpty());
}

// ── applyTextAnchor ─────────────────────────────────────────────────────────

TEST(ApplyTextAnchorTest, StartAnchorNoShift) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 100.0, .xAdvance = 10.0}, {.xPosition = 110.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textAnchor = TextAnchor::Start;
  text.spans.push_back(span);

  std::vector<ChunkBoundary> chunks = {{0, 0, TextAnchor::Start}};
  applyTextAnchor(runs, chunks, text, false);

  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(GlyphXPositionIs(DoubleEq(100.0)), GlyphXPositionIs(DoubleEq(110.0))));
}

TEST(ApplyTextAnchorTest, MiddleAnchorShiftsHalf) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 100.0, .xAdvance = 10.0}, {.xPosition = 110.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textAnchor = TextAnchor::Middle;
  text.spans.push_back(span);

  std::vector<ChunkBoundary> chunks = {{0, 0, TextAnchor::Middle}};
  applyTextAnchor(runs, chunks, text, false);

  // Chunk length = (110 + 10) - 100 = 20. Shift = -10.
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(GlyphXPositionIs(DoubleEq(90.0)), GlyphXPositionIs(DoubleEq(100.0))));
}

TEST(ApplyTextAnchorTest, EndAnchorShiftsFull) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 100.0, .xAdvance = 10.0}, {.xPosition = 110.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textAnchor = TextAnchor::End;
  text.spans.push_back(span);

  std::vector<ChunkBoundary> chunks = {{0, 0, TextAnchor::End}};
  applyTextAnchor(runs, chunks, text, false);

  // Chunk length = 20. Shift = -20.
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(GlyphXPositionIs(DoubleEq(80.0)), GlyphXPositionIs(DoubleEq(90.0))));
}

TEST(ApplyTextAnchorTest, VerticalModeShiftsYPosition) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.yPosition = 50.0, .yAdvance = 20.0}, {.yPosition = 70.0, .yAdvance = 20.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textAnchor = TextAnchor::End;
  text.spans.push_back(span);

  std::vector<ChunkBoundary> chunks = {{0, 0, TextAnchor::End}};
  applyTextAnchor(runs, chunks, text, true);

  // Chunk length = (70 + 20) - 50 = 40. Shift = -40.
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(GlyphYPositionIs(DoubleEq(10.0)), GlyphYPositionIs(DoubleEq(30.0))));
}

// ── applyTextLength ─────────────────────────────────────────────────────────

TEST(ApplyTextLengthTest, SpacingAdjustmentDistributesEvenly) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 0.0, .xAdvance = 10.0},
                    {.xPosition = 10.0, .xAdvance = 10.0},
                    {.xPosition = 20.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textLength = Lengthd(60.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::Spacing;
  text.spans.push_back(span);

  std::vector<RunPenExtent> extents = {{0.0, 0.0, 30.0, 0.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  applyTextLength(runs, text, extents, params, false, 30.0, 0.0);

  // Extra = 60 - 30 = 30. Per gap (2 gaps) = 15.
  EXPECT_THAT(runs[0].glyphs, ElementsAre(GlyphXPositionIs(DoubleNear(0.0, 0.001)),
                                          GlyphXPositionIs(DoubleNear(25.0, 0.001)),
                                          GlyphXPositionIs(DoubleNear(50.0, 0.001))));
}

TEST(ApplyTextLengthTest, SpacingAndScalingAdjustmentScalesPositions) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 0.0, .xAdvance = 10.0}, {.xPosition = 10.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textLength = Lengthd(40.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(span);

  std::vector<RunPenExtent> extents = {{0.0, 0.0, 20.0, 0.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  applyTextLength(runs, text, extents, params, false, 20.0, 0.0);

  // Scale factor = 40/20 = 2.0.
  EXPECT_THAT(
      runs[0].glyphs,
      ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(0.0, 0.001)), GlyphXAdvanceIs(DoubleNear(20.0, 0.001)),
                GlyphFontSizeScaleIs(FloatEq(1.0f)), GlyphStretchScaleXIs(FloatEq(2.0f)),
                GlyphStretchScaleYIs(FloatEq(1.0f))),
          AllOf(GlyphXPositionIs(DoubleNear(20.0, 0.001)), GlyphXAdvanceIs(DoubleNear(20.0, 0.001)),
                GlyphFontSizeScaleIs(FloatEq(1.0f)), GlyphStretchScaleXIs(FloatEq(2.0f)),
                GlyphStretchScaleYIs(FloatEq(1.0f)))));
}

TEST(ApplyTextLengthTest, GlobalTextLengthAppliesWhenNoSpanTextLength) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  text.spans.push_back(span);

  std::vector<RunPenExtent> extents = {{10.0, 0.0, 30.0, 0.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  params.textLength = Lengthd(40.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::Spacing;

  applyTextLength(runs, text, extents, params, false, 30.0, 0.0);

  // Global natural length = 30 - 10 = 20. Extra = 40 - 20 = 20. 1 gap = 20.
  EXPECT_THAT(runs[0].glyphs, ElementsAre(GlyphXPositionIs(DoubleNear(10.0, 0.001)),
                                          GlyphXPositionIs(DoubleNear(40.0, 0.001))));
}

TEST(ApplyTextLengthTest, VerticalPerSpanSpacingAndGlyphsScalesYPositions) {
  std::vector<TextRun> runs(1);
  runs[0].glyphs = {{.yPosition = 10.0, .yAdvance = 20.0}, {.yPosition = 30.0, .yAdvance = 20.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.textLength = Lengthd(80.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(span);

  std::vector<RunPenExtent> extents = {{0.0, 10.0, 0.0, 50.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  applyTextLength(runs, text, extents, params, true, 0.0, 50.0);

  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
          AllOf(GlyphYPositionIs(DoubleNear(10.0, 0.001)), GlyphYAdvanceIs(DoubleNear(40.0, 0.001)),
                GlyphStretchScaleYIs(FloatEq(2.0f))),
          AllOf(GlyphYPositionIs(DoubleNear(50.0, 0.001)), GlyphYAdvanceIs(DoubleNear(40.0, 0.001)),
                GlyphStretchScaleYIs(FloatEq(2.0f)))))));
}

TEST(ApplyTextLengthTest, GlobalSpacingAndGlyphsScalesAcrossRuns) {
  std::vector<TextRun> runs(2);
  runs[0].glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}};
  runs[1].glyphs = {{.xPosition = 20.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  text.spans.push_back(components::ComputedTextComponent::TextSpan());
  text.spans.push_back(components::ComputedTextComponent::TextSpan());

  std::vector<RunPenExtent> extents = {{10.0, 0.0, 20.0, 0.0}, {20.0, 0.0, 30.0, 0.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  params.textLength = Lengthd(60.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;

  applyTextLength(runs, text, extents, params, false, 30.0, 0.0);

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(10.0, 0.001)),
                                                         GlyphXAdvanceIs(DoubleNear(30.0, 0.001)),
                                                         GlyphStretchScaleXIs(FloatEq(3.0f))))),
                          RunGlyphsAre(ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(40.0, 0.001)),
                                                         GlyphXAdvanceIs(DoubleNear(30.0, 0.001)),
                                                         GlyphStretchScaleXIs(FloatEq(3.0f)))))));
}

TEST(ApplyTextLengthTest, IgnoresNonPositiveSpanLengths) {
  std::vector<TextRun> runs(2);
  runs[0].glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}};
  runs[1].glyphs = {{.xPosition = 50.0, .xAdvance = 10.0}};

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan zeroNaturalSpan;
  zeroNaturalSpan.textLength = Lengthd(40.0, Lengthd::Unit::None);
  components::ComputedTextComponent::TextSpan zeroTargetSpan;
  zeroTargetSpan.textLength = Lengthd(0.0, Lengthd::Unit::None);
  text.spans.push_back(zeroNaturalSpan);
  text.spans.push_back(zeroTargetSpan);

  std::vector<RunPenExtent> extents = {{10.0, 0.0, 10.0, 0.0}, {50.0, 0.0, 60.0, 0.0}};
  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  applyTextLength(runs, text, extents, params, false, 60.0, 0.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(10.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(50.0))))));
}

// ── computeSpanBaselineShiftPx ──────────────────────────────────────────────

TEST(ComputeSpanBaselineShiftPxTest, LengthBasedShift) {
  testing::NiceMock<MockTextBackend> backend;

  components::ComputedTextComponent::TextSpan span;
  span.baselineShift = Lengthd(10.0, Lengthd::Unit::Px);

  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  params.fontSize = Lengthd(16.0, Lengthd::Unit::Px);

  const double shift = computeSpanBaselineShiftPx(backend, span, FontHandle{}, 1.0f, params);
  EXPECT_DOUBLE_EQ(shift, 10.0);
}

TEST(ComputeSpanBaselineShiftPxTest, SubKeywordUsesOS2Metrics) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  testing::NiceMock<MockTextBackend> backend;
  ON_CALL(backend, subSuperMetrics(testing::_))
      .WillByDefault(testing::Return(SubSuperMetrics{200, 300}));

  components::ComputedTextComponent::TextSpan span;
  span.baselineShiftKeyword = BSK::Sub;
  span.baselineShift = Lengthd(-0.33, Lengthd::Unit::Em);

  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  params.fontSize = Lengthd(16.0, Lengthd::Unit::Px);

  // Scale = 1.0, subscriptYOffset = 200, so shift = -200 * 1.0 = -200.
  const double shift = computeSpanBaselineShiftPx(backend, span, FontHandle{}, 1.0f, params);
  EXPECT_DOUBLE_EQ(shift, -200.0);
}

TEST(ComputeSpanBaselineShiftPxTest, SuperKeywordUsesOS2Metrics) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  testing::NiceMock<MockTextBackend> backend;
  ON_CALL(backend, subSuperMetrics(testing::_))
      .WillByDefault(testing::Return(SubSuperMetrics{200, 300}));

  components::ComputedTextComponent::TextSpan span;
  span.baselineShiftKeyword = BSK::Super;
  span.baselineShift = Lengthd(0.4, Lengthd::Unit::Em);

  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  params.fontSize = Lengthd(16.0, Lengthd::Unit::Px);

  // Scale = 1.0, superscriptYOffset = 300, so shift = 300 * 1.0 = 300.
  const double shift = computeSpanBaselineShiftPx(backend, span, FontHandle{}, 1.0f, params);
  EXPECT_DOUBLE_EQ(shift, 300.0);
}

TEST(ComputeSpanBaselineShiftPxTest, SubKeywordFallsBackWhenMetricsAreMissing) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  testing::NiceMock<MockTextBackend> backend;
  ON_CALL(backend, subSuperMetrics(testing::_)).WillByDefault(testing::Return(std::nullopt));

  components::ComputedTextComponent::TextSpan span;
  span.baselineShiftKeyword = BSK::Sub;
  span.baselineShift = Lengthd(-0.25, Lengthd::Unit::Em);

  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics::DefaultsWithFontSize(20.0);
  params.fontSize = Lengthd(20.0, Lengthd::Unit::Px);

  const double shift = computeSpanBaselineShiftPx(backend, span, FontHandle{}, 1.0f, params);
  EXPECT_DOUBLE_EQ(shift, -5.0);
}

TEST(ComputeSpanBaselineShiftPxTest, AccumulatesAncestorKeywordAndLengthShifts) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  testing::NiceMock<MockTextBackend> backend;
  ON_CALL(backend, subSuperMetrics(testing::_))
      .WillByDefault(testing::Return(SubSuperMetrics{200, 300}));
  ON_CALL(backend, scaleForEmToPixels(testing::_, testing::_))
      .WillByDefault(testing::Return(0.02f));

  components::ComputedTextComponent::TextSpan span;
  span.baselineShift = Lengthd(5.0, Lengthd::Unit::Px);
  span.ancestorBaselineShifts.push_back({BSK::Sub, Lengthd(-0.33, Lengthd::Unit::Em), 18.0});
  span.ancestorBaselineShifts.push_back({BSK::Super, Lengthd(0.4, Lengthd::Unit::Em), 18.0});
  span.ancestorBaselineShifts.push_back({BSK::Length, Lengthd(3.0, Lengthd::Unit::Px), 18.0});

  TextLayoutParams params;
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics::DefaultsWithFontSize(20.0);
  params.fontSize = Lengthd(20.0, Lengthd::Unit::Px);

  const double shift = computeSpanBaselineShiftPx(backend, span, FontHandle{}, 1.0f, params);
  EXPECT_NEAR(shift, 10.0, 1e-6);
}

// ── TextEngine::layout() with MockTextBackend ───────────────────────────────

class TextEngineLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    auto mockBackend = std::make_unique<testing::NiceMock<MockTextBackend>>();
    mockBackend_ = mockBackend.get();
    engine_ = std::make_unique<TextEngine>(fontManager_, registry_, std::move(mockBackend));

    // Default mock behavior: simple metrics and scaling.
    ON_CALL(*mockBackend_, fontVMetrics(testing::_))
        .WillByDefault(testing::Return(FontVMetrics{800, -200, 0}));
    ON_CALL(*mockBackend_, scaleForPixelHeight(testing::_, testing::_))
        .WillByDefault(testing::Return(0.016f));  // 16px / 1000 upem
    ON_CALL(*mockBackend_, scaleForEmToPixels(testing::_, testing::_))
        .WillByDefault(testing::Return(0.016f));
    ON_CALL(*mockBackend_, isCursive(testing::_)).WillByDefault(testing::Return(false));
    ON_CALL(*mockBackend_, hasSmallCapsFeature(testing::_)).WillByDefault(testing::Return(false));
    ON_CALL(*mockBackend_, isBitmapOnly(testing::_)).WillByDefault(testing::Return(false));
    ON_CALL(*mockBackend_, subSuperMetrics(testing::_))
        .WillByDefault(testing::Return(std::nullopt));
    ON_CALL(*mockBackend_, crossSpanKern(testing::_, testing::_, testing::_, testing::_, testing::_,
                                         testing::_, testing::_))
        .WillByDefault(testing::Return(0.0));
  }

  /// Create a simple ShapedRun with N glyphs of fixed advance.
  /// Cluster values are byte offsets relative to the full span text (offset-based).
  static TextBackend::ShapedRun makeShapedRun(std::string_view spanText, size_t byteOffset,
                                              size_t byteLength, double advance) {
    TextBackend::ShapedRun run;
    size_t pos = byteOffset;
    const size_t end = byteOffset + byteLength;
    int glyphIdx = 1;
    while (pos < end) {
      TextBackend::ShapedGlyph g;
      g.glyphIndex = glyphIdx++;
      g.cluster = static_cast<uint32_t>(pos);
      g.xAdvance = advance;
      // Advance past the UTF-8 codepoint.
      const uint8_t byte = static_cast<uint8_t>(spanText[pos]);
      if ((byte & 0x80) == 0) {
        pos += 1;
      } else if ((byte & 0xE0) == 0xC0) {
        pos += 2;
      } else if ((byte & 0xF0) == 0xE0) {
        pos += 3;
      } else {
        pos += 4;
      }
      run.glyphs.push_back(g);
    }
    return run;
  }

  components::ComputedTextComponent::TextSpan makeSpan(const std::string& str) {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString(str);
    span.start = 0;
    span.end = str.size();
    span.startsNewChunk = true;
    return span;
  }

  TextLayoutParams makeParams(double fontSize = 16.0) {
    TextLayoutParams params;
    params.fontSize = Lengthd(fontSize, Lengthd::Unit::Px);
    params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
    params.fontMetrics = FontMetrics();
    return params;
  }

  Registry registry_;
  FontManager fontManager_{registry_};
  testing::NiceMock<MockTextBackend>* mockBackend_ = nullptr;
  std::unique_ptr<TextEngine> engine_;
};

TEST_F(TextEngineLayoutTest, BasicHorizontalLayout) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("ABC");
  span.xList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(50.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(60.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(70.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, PerCharacterXPositioning) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList = {Lengthd(10.0, Lengthd::Unit::None), Lengthd(50.0, Lengthd::Unit::None)};
  span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(10.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(50.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, TextAnchorMiddleShifts) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  span.textAnchor = TextAnchor::Middle;
  text.spans.push_back(std::move(span));

  auto params = makeParams();
  params.textAnchor = TextAnchor::Middle;
  const auto runs = engine_->layout(text, params);

  // Total advance = 20. Middle shift = -10.
  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(90.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(100.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, PerCharacterRotation) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.rotateList = {45.0, 90.0};
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(
                        ElementsAre(GlyphRotateDegreesIs(45.0), GlyphRotateDegreesIs(90.0)))));
}

TEST_F(TextEngineLayoutTest, LetterSpacingAddsSpace) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.letterSpacingPx = 5.0;
  text.spans.push_back(std::move(span));

  auto params = makeParams();
  params.letterSpacingPx = 5.0;
  const auto runs = engine_->layout(text, params);

  // Second glyph should be at advance + letter-spacing = 10 + 5 = 15.
  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(testing::_, GlyphXPositionIs(DoubleNear(15.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, CursiveGlyphSuppressesLetterSpacing) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });
  ON_CALL(*mockBackend_, isCursive(static_cast<uint32_t>('A')))
      .WillByDefault(testing::Return(true));

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.letterSpacingPx = 5.0;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(_, GlyphXPositionIs(DoubleNear(10.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, WordSpacingAddsAfterAsciiSpace) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("A B");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.wordSpacingPx = 5.0;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(0.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(10.0, 0.1)),
                                                   GlyphXPositionIs(DoubleNear(25.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, CrossSpanKerningAppliesWhenSpanContinuesChunk) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });
  ON_CALL(*mockBackend_,
          crossSpanKern(testing::_, testing::_, testing::_, testing::_, static_cast<uint32_t>('A'),
                        static_cast<uint32_t>('V'), false))
      .WillByDefault(testing::Return(-2.0));

  components::ComputedTextComponent text;
  auto span1 = makeSpan("A");
  span1.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span1.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  auto span2 = makeSpan("V");
  span2.startsNewChunk = false;
  text.spans.push_back(std::move(span1));
  text.spans.push_back(std::move(span2));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(1u)),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(8.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, VerticalLatinRotatesAndUsesSpacing) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("A B");
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(10.0, Lengthd::Unit::None));
  span.rotateList = {15.0};
  span.letterSpacingPx = 2.0;
  span.wordSpacingPx = 3.0;
  text.spans.push_back(std::move(span));

  auto params = makeParams();
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine_->layout(text, params);

  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(95.2, 0.1)), GlyphYPositionIs(DoubleNear(10.0, 0.1)),
                GlyphRotateDegreesIs(DoubleEq(105.0))),
          GlyphYPositionIs(DoubleNear(22.0, 0.1)), GlyphYPositionIs(DoubleNear(37.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, VerticalCjkUsesBackendOffsetsAndAdvanceFallback) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault(
          [](FontHandle, float, std::string_view, size_t offset, size_t, bool, FontVariant, bool) {
            TextBackend::ShapedRun run;
            run.glyphs.push_back(TextBackend::ShapedGlyph{
                .glyphIndex = 7,
                .xAdvance = 0.0,
                .yAdvance = 0.0,
                .xOffset = 3.0,
                .yOffset = -4.0,
                .cluster = static_cast<uint32_t>(offset),
            });
            return run;
          });

  components::ComputedTextComponent text;
  auto span = makeSpan("\xE6\x97\xA5");  // 日.
  span.xList.push_back(Lengthd(80.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(25.0, Lengthd::Unit::None));
  span.rotateList = {12.0};
  text.spans.push_back(std::move(span));

  auto params = makeParams();
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine_->layout(text, params);

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(AllOf(
                  GlyphIndexIs(7u), GlyphXPositionIs(DoubleNear(83.0, 0.1)),
                  GlyphYPositionIs(DoubleNear(21.0, 0.1)), GlyphYAdvanceIs(DoubleNear(16.0, 0.1)),
                  GlyphRotateDegreesIs(DoubleEq(12.0)))))));
}

TEST_F(TextEngineLayoutTest, VerticalPerCharacterAbsolutePositionsApplyDxDy) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList = {Lengthd(100.0, Lengthd::Unit::None), Lengthd(130.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(10.0, Lengthd::Unit::None), Lengthd(50.0, Lengthd::Unit::None)};
  span.dxList = {std::nullopt, Lengthd(3.0, Lengthd::Unit::None)};
  span.dyList = {std::nullopt, Lengthd(4.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  auto params = makeParams();
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine_->layout(text, params);

  EXPECT_THAT(
      runs, ElementsAre(RunGlyphsAre(ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(95.2, 0.1)),
                                                       GlyphYPositionIs(DoubleNear(10.0, 0.1))),
                                                 AllOf(GlyphXPositionIs(DoubleNear(128.2, 0.1)),
                                                       GlyphYPositionIs(DoubleNear(54.0, 0.1)))))));
}

TEST_F(TextEngineLayoutTest, HorizontalRtlChunkUsesAbsoluteYOverrideForWholeChunk) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        if (offset == 2 && length == 2) {
          TextBackend::ShapedRun run;
          run.glyphs.push_back(TextBackend::ShapedGlyph{
              .glyphIndex = 3,
              .xAdvance = 10.0,
              .cluster = 3,
          });
          run.glyphs.push_back(TextBackend::ShapedGlyph{
              .glyphIndex = 2,
              .xAdvance = 10.0,
              .cluster = 2,
          });
          return run;
        }
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("ABCD");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList = {Lengthd(10.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(50.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
          GlyphYPositionIs(DoubleNear(10.0, 0.1)), GlyphYPositionIs(DoubleNear(10.0, 0.1)),
          GlyphYPositionIs(DoubleNear(50.0, 0.1)), GlyphYPositionIs(DoubleNear(50.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, AlignmentBaselineHangingShiftsGlyphsDown) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  // Effective baseline (dominant-baseline or alignment-baseline override) resolved per span.
  span.alignmentBaseline = DominantBaseline::Hanging;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  // Hanging shift = 0.8 * ascent(800) * emScale(0.016) = 10.24, added to y (glyphs move down).
  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(60.24, 0.01)),
                                                   GlyphYPositionIs(DoubleNear(60.24, 0.01))))));
}

TEST_F(TextEngineLayoutTest, BaselineAlignmentIsIgnoredInVerticalWritingMode) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  auto layoutFirstGlyphPosition = [&](DominantBaseline effectiveBaseline) {
    components::ComputedTextComponent text;
    auto span = makeSpan("AB");
    span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(20.0, Lengthd::Unit::None));
    span.alignmentBaseline = effectiveBaseline;
    text.spans.push_back(std::move(span));

    auto params = makeParams();
    params.writingMode = WritingMode::VerticalRl;
    const auto runs = engine_->layout(text, params);
    EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(2))));
    if (runs.empty() || runs[0].glyphs.empty()) {
      return Vector2d();
    }
    return Vector2d(runs[0].glyphs[0].xPosition, runs[0].glyphs[0].yPosition);
  };

  const Vector2d hangingPosition = layoutFirstGlyphPosition(DominantBaseline::Hanging);
  const Vector2d autoPosition = layoutFirstGlyphPosition(DominantBaseline::Auto);
  EXPECT_THAT(hangingPosition.x, testing::DoubleEq(autoPosition.x));
  EXPECT_THAT(hangingPosition.y, testing::DoubleEq(autoPosition.y));
}

TEST_F(TextEngineLayoutTest, VisibilityHiddenAdvancesButClearsGlyphs) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto hiddenSpan = makeSpan("AB");
  hiddenSpan.xList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  hiddenSpan.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
  hiddenSpan.visibility = Visibility::Hidden;
  auto visibleSpan = makeSpan("C");
  visibleSpan.startsNewChunk = false;
  text.spans.push_back(std::move(hiddenSpan));
  text.spans.push_back(std::move(visibleSpan));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(IsEmpty()),
                          RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(20.0, 0.1))))));
}

TEST_F(TextEngineLayoutTest, TextPathFailureProducesEmptyRunWithoutShaping) {
  EXPECT_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                      testing::_, testing::_, testing::_))
      .Times(0);

  components::ComputedTextComponent text;
  auto span = makeSpan("missing path");
  span.textPathFailed = true;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty())));
}

TEST_F(TextEngineLayoutTest, HiddenSpanProducesEmptyGlyphs) {
  components::ComputedTextComponent text;
  auto span = makeSpan("ABC");
  span.hidden = true;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty())));
}

TEST_F(TextEngineLayoutTest, TextPathVisibilityHiddenClearsGlyphsAfterPathLayout) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.pathSpline = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(100.0, 0.0)).build();
  span.visibility = Visibility::Hidden;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(AllOf(Field("onPath", &TextRun::onPath, true), RunGlyphsAre(IsEmpty()))));
}

TEST_F(TextEngineLayoutTest, EmptyTextPathHidesGlyphsAndLeavesPathRun) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;
  auto span = makeSpan("AB");
  span.pathSpline = Path{};
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(AllOf(Field("onPath", &TextRun::onPath, true),
                                RunGlyphsAre(ElementsAre(GlyphIndexIs(0u), GlyphIndexIs(0u))))));
}

TEST_F(TextEngineLayoutTest, EmptySpanPreservesPosition) {
  ON_CALL(*mockBackend_, shapeRun(testing::_, testing::_, testing::_, testing::_, testing::_,
                                  testing::_, testing::_, testing::_))
      .WillByDefault([](FontHandle, float, std::string_view text, size_t offset, size_t length,
                        bool, FontVariant, bool) {
        return TextEngineLayoutTest::makeShapedRun(text, offset, length, 10.0);
      });

  components::ComputedTextComponent text;

  // First span: empty, sets position.
  auto span1 = makeSpan("");
  span1.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span1.yList.push_back(Lengthd(50.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span1));

  // Second span: continues from first span's position.
  auto span2 = makeSpan("A");
  span2.startsNewChunk = false;
  text.spans.push_back(std::move(span2));

  const auto runs = engine_->layout(text, makeParams());

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(IsEmpty()),
                          RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(100.0, 0.1))))));
}

// ── Effective baseline resolution (resolvePerSpanLayoutStyles) ──────────────

/// Matches a TextSpan whose text is \p expectedText and whose resolved effective baseline
/// (TextSpan::alignmentBaseline) is \p expectedBaseline. On mismatch, prints the span's text
/// and effective baseline keyword.
MATCHER_P2(SpanWithEffectiveBaseline, expectedText, expectedBaseline,
           std::string("span with text '") + expectedText + "' and effective baseline '" +
               testing::PrintToString(expectedBaseline) + "'") {
  const std::string_view spanText(arg.text.data() + arg.start, arg.end - arg.start);
  if (spanText != expectedText) {
    *result_listener << "span text is '" << spanText << "'";
    return false;
  }
  if (arg.alignmentBaseline != expectedBaseline) {
    *result_listener << "effective baseline is '" << arg.alignmentBaseline << "'";
    return false;
  }
  return true;
}

class EffectiveBaselineResolutionTest : public testing::Test {
protected:
  /// Parses \p svg, computes styles and text spans, and runs the per-span layout-style
  /// resolution that folds dominant-baseline / alignment-baseline into each span's
  /// effective baseline. Returns the resolved spans of the element with id="t".
  SmallVector<components::ComputedTextComponent::TextSpan, 1> resolveSpans(std::string_view svg) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(svg, parseSink);
    EXPECT_FALSE(maybeResult.hasError()) << "SVG parse failed";
    document_ = std::make_unique<SVGDocument>(std::move(maybeResult).result());

    Registry& registry = document_->registry();

    auto maybeText = document_->querySelector("#t");
    EXPECT_TRUE(maybeText.has_value()) << "No element with id=\"t\"";
    const EntityHandle textRootHandle = maybeText->unsafeEntityHandle();

    fontManager_ = std::make_unique<FontManager>(registry);
    engine_ = std::make_unique<TextEngine>(*fontManager_, registry);

    // Compute styles + text spans through the production path, then resolve per-span
    // layout styles (the step that folds dominant/alignment-baseline per span).
    ParseWarningSink warningSink;
    engine_->prepareForElement(textRootHandle, warningSink);

    auto* computed = registry.try_get<components::ComputedTextComponent>(textRootHandle.entity());
    EXPECT_NE(computed, nullptr);
    if (!computed) {
      return {};
    }

    engine_->resolvePerSpanLayoutStyles(textRootHandle, *computed);
    return computed->spans;
  }

  std::unique_ptr<SVGDocument> document_;
  std::unique_ptr<FontManager> fontManager_;
  std::unique_ptr<TextEngine> engine_;
};

TEST_F(EffectiveBaselineResolutionTest, DominantBaselineInheritsToTspan) {
  const auto spans = resolveSpans(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20" dominant-baseline="middle"><tspan>A</tspan></text>
    </svg>
  )");

  EXPECT_THAT(spans, testing::Contains(SpanWithEffectiveBaseline("A", DominantBaseline::Middle)));
}

TEST_F(EffectiveBaselineResolutionTest, AlignmentBaselineOverridesDominantBaseline) {
  const auto spans = resolveSpans(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20" dominant-baseline="middle">
        <tspan alignment-baseline="hanging">A</tspan>
      </text>
    </svg>
  )");

  EXPECT_THAT(spans, testing::Contains(SpanWithEffectiveBaseline("A", DominantBaseline::Hanging)));
}

TEST_F(EffectiveBaselineResolutionTest, AlignmentBaselineBaselineKeywordDefersToDominant) {
  // `alignment-baseline: baseline` parses to Auto, so the span's own dominant-baseline
  // (here `hanging`, set on the tspan) applies - matching resvg and Chrome.
  const auto spans = resolveSpans(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20" dominant-baseline="middle">
        <tspan dominant-baseline="hanging" alignment-baseline="baseline">A</tspan>
      </text>
    </svg>
  )");

  EXPECT_THAT(spans, testing::Contains(SpanWithEffectiveBaseline("A", DominantBaseline::Hanging)));
}

TEST_F(EffectiveBaselineResolutionTest, NoChangeUsesParentDominantBaseline) {
  const auto spans = resolveSpans(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20" dominant-baseline="middle">
        <tspan dominant-baseline="no-change">A</tspan>
      </text>
    </svg>
  )");

  EXPECT_THAT(spans, testing::Contains(SpanWithEffectiveBaseline("A", DominantBaseline::Middle)));
}

TEST_F(EffectiveBaselineResolutionTest, UseScriptResolvesAsIs) {
  // `use-script` is deprecated and unsupported; it stays in the effective baseline and
  // computeBaselineShift() maps it to a zero shift (behaves like auto).
  const auto spans = resolveSpans(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <text id="t" x="10" y="20" dominant-baseline="use-script">A</text>
    </svg>
  )");

  EXPECT_THAT(spans,
              testing::Contains(SpanWithEffectiveBaseline("A", DominantBaseline::UseScript)));
}

}  // namespace

}  // namespace donner::svg
