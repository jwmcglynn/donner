#include "donner/svg/text/TextEngineHelpers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

// ── computeBaselineShift ────────────────────────────────────────────────────

TEST(ComputeBaselineShiftTest, AutoReturnsZero) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Auto, vm, 1.0f), 0.0);
}

TEST(ComputeBaselineShiftTest, AlphabeticReturnsZero) {
  FontVMetrics vm{800, -200, 0};
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Alphabetic, vm, 1.0f), 0.0);
}

TEST(ComputeBaselineShiftTest, MiddleCentersEmBox) {
  FontVMetrics vm{800, -200, 0};
  // (800 + (-200)) * 0.5 * 1.0 = 300.0
  EXPECT_DOUBLE_EQ(computeBaselineShift(DominantBaseline::Middle, vm, 1.0f), 300.0);
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
  ASSERT_EQ(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 5u);
}

TEST(FindChunkRangesTest, SplitsAtAbsoluteXPosition) {
  SmallVector<std::optional<Lengthd>, 1> xList = {std::nullopt, std::nullopt,
                                                  Lengthd(50.0, Lengthd::Unit::None)};
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("ABC", xList, yList);
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 2u);
  EXPECT_EQ(ranges[1].byteStart, 2u);
  EXPECT_EQ(ranges[1].byteEnd, 3u);
}

TEST(FindChunkRangesTest, SplitsAtAbsoluteYPosition) {
  SmallVector<std::optional<Lengthd>, 1> xList;
  SmallVector<std::optional<Lengthd>, 1> yList = {std::nullopt,
                                                  Lengthd(100.0, Lengthd::Unit::None)};
  const auto ranges = findChunkRanges("AB", xList, yList);
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 1u);
  EXPECT_EQ(ranges[1].byteStart, 1u);
  EXPECT_EQ(ranges[1].byteEnd, 2u);
}

TEST(FindChunkRangesTest, HandlesMultibyteUtf8) {
  // "Aé" = 'A'(1 byte) + 'é'(2 bytes: 0xC3 0xA9)
  SmallVector<std::optional<Lengthd>, 1> xList = {std::nullopt, Lengthd(50.0, Lengthd::Unit::None)};
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("A\xC3\xA9", xList, yList);
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 1u);
  EXPECT_EQ(ranges[1].byteStart, 1u);
  EXPECT_EQ(ranges[1].byteEnd, 3u);
}

TEST(FindChunkRangesTest, EmptyTextReturnsSingleEmptyRange) {
  SmallVector<std::optional<Lengthd>, 1> xList;
  SmallVector<std::optional<Lengthd>, 1> yList;
  const auto ranges = findChunkRanges("", xList, yList);
  ASSERT_EQ(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].byteStart, 0u);
  EXPECT_EQ(ranges[0].byteEnd, 0u);
}

// ── buildByteIndexMappings ──────────────────────────────────────────────────

TEST(BuildByteIndexMappingsTest, AsciiText) {
  const auto m = buildByteIndexMappings("ABC");
  ASSERT_EQ(m.byteToCharIdx.size(), 3u);
  EXPECT_EQ(m.byteToCharIdx[0], 0u);
  EXPECT_EQ(m.byteToCharIdx[1], 1u);
  EXPECT_EQ(m.byteToCharIdx[2], 2u);
  EXPECT_EQ(m.byteToRawCpIdx[0], 0u);
  EXPECT_EQ(m.byteToRawCpIdx[1], 1u);
  EXPECT_EQ(m.byteToRawCpIdx[2], 2u);
}

TEST(BuildByteIndexMappingsTest, CombiningMarkSharesBaseIndex) {
  // "o" + combining low line (U+0332, 2 bytes: CC B2)
  const auto m = buildByteIndexMappings("o\xCC\xB2");
  ASSERT_EQ(m.byteToCharIdx.size(), 3u);
  EXPECT_EQ(m.byteToCharIdx[0], 0u);  // 'o'
  EXPECT_EQ(m.byteToCharIdx[1], 0u);  // combining mark byte 1 shares base index
  EXPECT_EQ(m.byteToCharIdx[2], 0u);  // combining mark byte 2 shares base index
}

TEST(BuildByteIndexMappingsTest, SupplementaryCharacterConsumeTwoIndices) {
  // "A" + U+1F601 (😁, 4 bytes) + "B". Supplementary chars increment charIdx by 2
  // (UTF-16 surrogate pair semantics) when they're not the first character.
  const auto m = buildByteIndexMappings(
      "A\xF0\x9F\x98\x81"
      "B");
  ASSERT_EQ(m.byteToCharIdx.size(), 6u);
  // 'A' at byte 0 → charIdx 0 (first char).
  EXPECT_EQ(m.byteToCharIdx[0], 0u);
  // Emoji at bytes 1..4 → charIdx 2 (non-first supplementary, +2).
  EXPECT_EQ(m.byteToCharIdx[1], 2u);
  EXPECT_EQ(m.byteToCharIdx[4], 2u);
  // 'B' at byte 5 → charIdx 3 (non-first BMP, +1).
  EXPECT_EQ(m.byteToCharIdx[5], 3u);
}

TEST(BuildByteIndexMappingsTest, EmptyText) {
  const auto m = buildByteIndexMappings("");
  EXPECT_TRUE(m.byteToCharIdx.empty());
  EXPECT_TRUE(m.byteToRawCpIdx.empty());
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

  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 100.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 110.0);
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
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 90.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 100.0);
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
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 80.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 90.0);
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
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 10.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 30.0);
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
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 0.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 25.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[2].xPosition, 50.0, 0.001);
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
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 0.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 20.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[0].xAdvance, 20.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xAdvance, 20.0, 0.001);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].fontSizeScale, 1.0f);
  EXPECT_FLOAT_EQ(runs[0].glyphs[1].fontSizeScale, 1.0f);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].stretchScaleX, 2.0f);
  EXPECT_FLOAT_EQ(runs[0].glyphs[1].stretchScaleX, 2.0f);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].stretchScaleY, 1.0f);
  EXPECT_FLOAT_EQ(runs[0].glyphs[1].stretchScaleY, 1.0f);
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
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 10.0, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 40.0, 0.001);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 3u);
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 50.0, 0.1);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 60.0, 0.1);
  EXPECT_NEAR(runs[0].glyphs[2].xPosition, 70.0, 0.1);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 10.0, 0.1);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 50.0, 0.1);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  // Total advance = 20. Middle shift = -10.
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, 90.0, 0.1);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 100.0, 0.1);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].rotateDegrees, 45.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].rotateDegrees, 90.0);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  // Second glyph should be at advance + letter-spacing = 10 + 5 = 15.
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 15.0, 0.1);
}

TEST_F(TextEngineLayoutTest, HiddenSpanProducesEmptyGlyphs) {
  components::ComputedTextComponent text;
  auto span = makeSpan("ABC");
  span.hidden = true;
  text.spans.push_back(std::move(span));

  const auto runs = engine_->layout(text, makeParams());

  ASSERT_EQ(runs.size(), 1u);
  EXPECT_TRUE(runs[0].glyphs.empty());
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

  ASSERT_EQ(runs.size(), 2u);
  EXPECT_TRUE(runs[0].glyphs.empty());
  ASSERT_EQ(runs[1].glyphs.size(), 1u);
  EXPECT_NEAR(runs[1].glyphs[0].xPosition, 100.0, 0.1);
}

}  // namespace

}  // namespace donner::svg
