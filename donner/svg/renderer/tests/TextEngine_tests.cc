#include "donner/svg/text/TextEngine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextEngineHelpers.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {

namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FloatEq;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Lt;
using ::testing::Not;
using ::testing::SizeIs;

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
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  return params;
}

FontHandle LoadResvgFont(FontManager& fontManager, const std::string& fontFilename,
                         const std::string& familyName) {
  const std::string fontsDir = Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts");
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

auto GlyphIndexIs(auto matcher) {
  return Field("glyphIndex", &TextGlyph::glyphIndex, matcher);
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

auto GlyphRotateDegreesIs(auto matcher) {
  return Field("rotateDegrees", &TextGlyph::rotateDegrees, matcher);
}

auto GlyphStretchScaleXIs(auto matcher) {
  return Field("stretchScaleX", &TextGlyph::stretchScaleX, matcher);
}

auto GlyphStretchScaleYIs(auto matcher) {
  return Field("stretchScaleY", &TextGlyph::stretchScaleY, matcher);
}

auto RunFontIs(auto matcher) {
  return Field("font", &TextRun::font, matcher);
}

auto RunGlyphsAre(auto matcher) {
  return Field("glyphs", &TextRun::glyphs, matcher);
}

auto ChunkRangeIs(std::size_t byteStart, std::size_t byteEnd) {
  return AllOf(Field("byteStart", &text_engine_detail::ChunkRange::byteStart, byteStart),
               Field("byteEnd", &text_engine_detail::ChunkRange::byteEnd, byteEnd));
}

auto ChunkTextAnchorIs(TextAnchor textAnchor) {
  return Field("textAnchor", &text_engine_detail::ChunkBoundary::textAnchor, textAnchor);
}

MATCHER_P(FirstGlyphMatches, glyphMatcher, "first glyph matches") {
  if (arg.empty()) {
    *result_listener << "glyph list is empty";
    return false;
  }

  return testing::ExplainMatchResult(glyphMatcher, arg.front(), result_listener);
}

class FakeTextBackend : public TextBackend {
public:
  std::optional<SubSuperMetrics> subSuperMetricsResult;

  FontVMetrics fontVMetrics(FontHandle /*font*/) const override { return {}; }

  float scaleForPixelHeight(FontHandle /*font*/, float pixelHeight) const override {
    return pixelHeight / 1000.0f;
  }

  float scaleForEmToPixels(FontHandle /*font*/, float pixelHeight) const override {
    return pixelHeight / 1000.0f;
  }

  std::optional<UnderlineMetrics> underlineMetrics(FontHandle /*font*/) const override {
    return std::nullopt;
  }

  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle /*font*/) const override {
    return std::nullopt;
  }

  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle /*font*/) const override {
    return subSuperMetricsResult;
  }

  Path glyphOutline(FontHandle /*font*/, int /*glyphIndex*/, float /*scale*/) const override {
    return Path();
  }

  bool isBitmapOnly(FontHandle /*font*/) const override { return false; }

  bool isCursive(uint32_t /*codepoint*/) const override { return false; }

  bool hasSmallCapsFeature(FontHandle /*font*/) const override { return false; }

  std::optional<BitmapGlyph> bitmapGlyph(FontHandle /*font*/, int /*glyphIndex*/,
                                         float /*scale*/) const override {
    return std::nullopt;
  }

  ShapedRun shapeRun(FontHandle /*font*/, float /*fontSizePx*/, std::string_view /*spanText*/,
                     size_t /*byteOffset*/, size_t /*byteLength*/, bool /*isVertical*/,
                     FontVariant /*fontVariant*/, bool /*forceLogicalOrder*/) const override {
    return {};
  }

  double crossSpanKern(FontHandle /*prevFont*/, float /*prevSizePx*/, FontHandle /*curFont*/,
                       float /*curSizePx*/, uint32_t /*prevCodepoint*/, uint32_t /*curCodepoint*/,
                       bool /*isVertical*/) const override {
    return 0.0;
  }
};

class ScriptedTextBackend : public TextBackend {
public:
  bool reverseClusters = false;

  FontVMetrics fontVMetrics(FontHandle /*font*/) const override {
    return FontVMetrics{
        .ascent = 1000,
        .descent = -200,
        .lineGap = 0,
        .xHeight = 500,
    };
  }

  float scaleForPixelHeight(FontHandle /*font*/, float pixelHeight) const override {
    return pixelHeight / 1200.0f;
  }

  float scaleForEmToPixels(FontHandle /*font*/, float pixelHeight) const override {
    return pixelHeight / 1000.0f;
  }

  std::optional<UnderlineMetrics> underlineMetrics(FontHandle /*font*/) const override {
    return UnderlineMetrics{.position = -100.0, .thickness = 50.0};
  }

  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle /*font*/) const override {
    return UnderlineMetrics{.position = 300.0, .thickness = 40.0};
  }

  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle /*font*/) const override {
    return std::nullopt;
  }

  Path glyphOutline(FontHandle /*font*/, int glyphIndex, float /*scale*/) const override {
    if (glyphIndex == 0) {
      return Path();
    }
    return PathBuilder().addRect(Box2d::FromXYWH(0.0, -8.0, 6.0, 8.0)).build();
  }

  bool isBitmapOnly(FontHandle /*font*/) const override { return false; }

  bool isCursive(uint32_t codepoint) const override { return codepoint == U'c'; }

  bool hasSmallCapsFeature(FontHandle /*font*/) const override { return false; }

  std::optional<BitmapGlyph> bitmapGlyph(FontHandle /*font*/, int /*glyphIndex*/,
                                         float /*scale*/) const override {
    return std::nullopt;
  }

  ShapedRun shapeRun(FontHandle /*font*/, float /*fontSizePx*/, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool /*isVertical*/,
                     FontVariant fontVariant, bool forceLogicalOrder) const override {
    ShapedRun run;
    const size_t byteEnd = std::min(spanText.size(), byteOffset + byteLength);
    for (size_t pos = byteOffset; pos < byteEnd;) {
      const size_t cluster = pos;
      const auto [codepoint, codepointLength] = Utf8::NextCodepointLenient(spanText.substr(pos));
      pos += static_cast<size_t>(std::max(codepointLength, 1));

      TextBackend::ShapedGlyph glyph;
      glyph.glyphIndex = static_cast<int>(codepoint % 997u) + 1;
      glyph.xAdvance = 10.0;
      glyph.yAdvance = 12.0;
      glyph.xKern = run.glyphs.empty() ? 0.0 : 1.0;
      glyph.yKern = run.glyphs.empty() ? 0.0 : 2.0;
      glyph.cluster = static_cast<uint32_t>(cluster);
      glyph.fontSizeScale = fontVariant == FontVariant::SmallCaps ? 0.8f : 1.0f;
      if (codepoint >= 0x2E80) {
        glyph.xOffset = 2.0;
        glyph.yOffset = 3.0;
      }
      run.glyphs.push_back(glyph);
    }

    if (reverseClusters && !forceLogicalOrder) {
      std::reverse(run.glyphs.begin(), run.glyphs.end());
    }
    return run;
  }

  double crossSpanKern(FontHandle /*prevFont*/, float /*prevSizePx*/, FontHandle /*curFont*/,
                       float /*curSizePx*/, uint32_t /*prevCodepoint*/, uint32_t /*curCodepoint*/,
                       bool isVertical) const override {
    return isVertical ? 4.0 : 3.0;
  }
};

TextEngine MakeScriptedEngine(Registry& registry, FontManager& fontManager,
                              bool reverseClusters = false) {
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->reverseClusters = reverseClusters;
  return TextEngine(fontManager, registry, std::move(backend));
}

css::FontFace LoadResvgFontFace(const std::string& fontFilename, const std::string& familyName) {
  const std::string fontsDir = Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts");
  const std::string fontPath = fontsDir + "/" + fontFilename;

  std::ifstream file(fontPath, std::ios::binary);
  if (!file.good()) {
    ADD_FAILURE() << fontPath;
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
  return face;
}

}  // namespace

TEST(TextEngineTest, AddFontFaceRegistersFontWithManager) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  engine.addFontFace(LoadResvgFontFace("NotoSans-Regular.ttf", "EngineAddFontFace"));

  EXPECT_TRUE(fontManager.findFont(RcString("EngineAddFontFace")));
}

TEST(TextEngineTest, SubSuperMetricsForwardsBackendResult) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<FakeTextBackend>();
  backend->subSuperMetricsResult = SubSuperMetrics{
      .subscriptYOffset = 123,
      .superscriptYOffset = 456,
  };
  TextEngine engine(fontManager, registry, std::move(backend));

  const std::optional<SubSuperMetrics> metrics = engine.subSuperMetrics(FontHandle{});

  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->subscriptYOffset, 123);
  EXPECT_EQ(metrics->superscriptYOffset, 456);
}

TEST(TextEngineHelperTest, ComputeBaselineShiftCoversBaselineKeywords) {
  const FontVMetrics metrics{
      .ascent = 1000,
      .descent = -200,
      .lineGap = 0,
      .xHeight = 500,
  };
  constexpr float kScale = 0.01f;

  EXPECT_DOUBLE_EQ(
      text_engine_detail::computeBaselineShift(DominantBaseline::Auto, metrics, kScale), 0.0);
  EXPECT_DOUBLE_EQ(
      text_engine_detail::computeBaselineShift(DominantBaseline::Alphabetic, metrics, kScale), 0.0);
  EXPECT_DOUBLE_EQ(
      text_engine_detail::computeBaselineShift(DominantBaseline::UseScript, metrics, kScale), 0.0);
  EXPECT_DOUBLE_EQ(
      text_engine_detail::computeBaselineShift(DominantBaseline::NoChange, metrics, kScale), 0.0);
  EXPECT_DOUBLE_EQ(
      text_engine_detail::computeBaselineShift(DominantBaseline::ResetSize, metrics, kScale), 0.0);
  EXPECT_NEAR(text_engine_detail::computeBaselineShift(DominantBaseline::Middle, metrics, kScale),
              2.5, 1e-6);
  EXPECT_NEAR(text_engine_detail::computeBaselineShift(DominantBaseline::Central, metrics, kScale),
              4.0, 1e-6);
  EXPECT_NEAR(text_engine_detail::computeBaselineShift(DominantBaseline::Hanging, metrics, kScale),
              8.0, 1e-6);
  EXPECT_NEAR(
      text_engine_detail::computeBaselineShift(DominantBaseline::Mathematical, metrics, kScale),
      5.0, 1e-6);
  EXPECT_NEAR(text_engine_detail::computeBaselineShift(DominantBaseline::TextTop, metrics, kScale),
              10.0, 1e-6);
  EXPECT_NEAR(
      text_engine_detail::computeBaselineShift(DominantBaseline::TextBottom, metrics, kScale), -2.0,
      1e-6);
  EXPECT_NEAR(
      text_engine_detail::computeBaselineShift(DominantBaseline::Ideographic, metrics, kScale),
      -2.0, 1e-6);

  FontVMetrics noXHeight = metrics;
  noXHeight.xHeight = 0;
  EXPECT_NEAR(text_engine_detail::computeBaselineShift(DominantBaseline::Middle, noXHeight, kScale),
              2.7, 1e-6);
}

TEST(TextEngineHelperTest, ByteMappingsAndChunkRangesCollapseNonSpacingSequences) {
  std::string text;
  std::vector<size_t> offsets;
  auto append = [&](char32_t cp) {
    offsets.push_back(text.size());
    Utf8::Append(cp, std::back_inserter(text));
  };

  append(U'A');
  for (const char32_t cp :
       {U'\u0300', U'\u0483', U'\u0591', U'\u0610', U'\u064B', U'\u0670',    U'\u06D6',
        U'\u0730', U'\u0E31', U'\u0E34', U'\u0EB1', U'\u0EB4', U'\u1AB0',    U'\u1DC0',
        U'\u20D0', U'\uFE20', U'\u200C', U'\u034F', U'\uFE00', U'\U000E0100'}) {
    append(cp);
  }
  append(U'\u200D');
  append(U'B');
  const size_t cOffsetIndex = offsets.size();
  append(U'C');
  const size_t emojiOffsetIndex = offsets.size();
  append(U'\U0001F600');
  const size_t dOffsetIndex = offsets.size();
  append(U'D');

  const auto mappings = text_engine_detail::buildByteIndexMappings(text);
  for (size_t i = 0; i < cOffsetIndex; ++i) {
    EXPECT_EQ(mappings.byteToCharIdx[offsets[i]], 0u) << "offset index " << i;
  }
  EXPECT_EQ(mappings.byteToCharIdx[offsets[cOffsetIndex]], 1u);
  EXPECT_EQ(mappings.byteToCharIdx[offsets[emojiOffsetIndex]], 3u);
  EXPECT_EQ(mappings.byteToCharIdx[offsets[dOffsetIndex]], 4u);

  SmallVector<std::optional<Lengthd>, 1> xList;
  xList.push_back(std::nullopt);
  xList.push_back(Lengthd(10.0, Lengthd::Unit::None));
  xList.push_back(std::nullopt);
  xList.push_back(Lengthd(20.0, Lengthd::Unit::None));
  SmallVector<std::optional<Lengthd>, 1> yList;

  const auto chunks = text_engine_detail::findChunkRanges(text, xList, yList);

  EXPECT_THAT(chunks, ElementsAre(ChunkRangeIs(0u, offsets[cOffsetIndex]),
                                  ChunkRangeIs(offsets[cOffsetIndex], offsets[emojiOffsetIndex]),
                                  ChunkRangeIs(offsets[emojiOffsetIndex], text.size())));
}

TEST(TextEngineHelperTest, ApplyTextLengthAdjustsPerSpanSpacingAndVerticalGlyphScaling) {
  {
    std::vector<TextRun> runs = {
        {.glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}}},
    };
    components::ComputedTextComponent text;
    text.spans.resize(1);
    text.spans[0].textLength = Lengthd(40.0, Lengthd::Unit::None);
    text.spans[0].lengthAdjust = LengthAdjust::Spacing;
    const std::vector<text_engine_detail::RunPenExtent> extents = {
        {.startX = 10.0, .endX = 30.0},
    };

    text_engine_detail::applyTextLength(runs, text, extents, MakeTextParams(10.0),
                                        /*vertical=*/false, /*currentPenX=*/30.0,
                                        /*currentPenY=*/0.0);

    EXPECT_THAT(runs,
                ElementsAre(RunGlyphsAre(ElementsAre(
                    GlyphXPositionIs(DoubleEq(10.0)),
                    AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphXAdvanceIs(DoubleEq(10.0)))))));
  }

  {
    std::vector<TextRun> runs = {
        {.glyphs = {{.yPosition = 5.0, .yAdvance = 5.0}, {.yPosition = 15.0, .yAdvance = 5.0}}},
    };
    components::ComputedTextComponent text;
    text.spans.resize(1);
    text.spans[0].textLength = Lengthd(40.0, Lengthd::Unit::None);
    text.spans[0].lengthAdjust = LengthAdjust::SpacingAndGlyphs;
    const std::vector<text_engine_detail::RunPenExtent> extents = {
        {.startY = 5.0, .endY = 25.0},
    };

    text_engine_detail::applyTextLength(runs, text, extents, MakeTextParams(10.0),
                                        /*vertical=*/true, /*currentPenX=*/0.0,
                                        /*currentPenY=*/25.0);

    EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                          GlyphYPositionIs(DoubleEq(5.0)),
                          AllOf(GlyphYPositionIs(DoubleEq(25.0)), GlyphYAdvanceIs(DoubleEq(10.0)),
                                GlyphStretchScaleYIs(FloatEq(2.0f)))))));
  }
}

TEST(TextEngineHelperTest, ApplyTextLengthAdjustsGlobalSpacing) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}}},
      {.glyphs = {{.xPosition = 30.0, .xAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(2);
  TextLayoutParams params = MakeTextParams(10.0);
  params.textLength = Lengthd(50.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::Spacing;

  text_engine_detail::applyTextLength(runs, text, {}, params, /*vertical=*/false,
                                      /*currentPenX=*/40.0, /*currentPenY=*/0.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(10.0)),
                                                         GlyphXPositionIs(DoubleEq(30.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(50.0))))));
}

TEST(TextEngineHelperTest, ApplyTextLengthAdjustsGlobalSpacingAndGlyphsHorizontally) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}}},
      {.glyphs = {{.xPosition = 30.0, .xAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(2);
  TextLayoutParams params = MakeTextParams(10.0);
  params.textLength = Lengthd(60.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;

  text_engine_detail::applyTextLength(runs, text, {}, params, /*vertical=*/false,
                                      /*currentPenX=*/40.0, /*currentPenY=*/0.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(10.0)),
                                                               GlyphXAdvanceIs(DoubleEq(20.0)),
                                                               GlyphStretchScaleXIs(FloatEq(2.0f))),
                                                         GlyphXPositionIs(DoubleEq(30.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(50.0))))));
}

TEST(TextEngineHelperTest, ApplyTextAnchorSkipsOnPathRunsAndUsesFirstGlyphSpanAnchor) {
  std::vector<TextRun> runs = {
      {},
      {.glyphs = {{.xPosition = 10.0, .xAdvance = 5.0}}},
      {.glyphs = {{.xPosition = 100.0, .xAdvance = 10.0}}, .onPath = true},
  };
  components::ComputedTextComponent text;
  text.spans.resize(3);
  text.spans[1].textAnchor = TextAnchor::End;
  std::vector<text_engine_detail::ChunkBoundary> chunks = {
      {.runIndex = 0, .glyphIndex = 0, .textAnchor = TextAnchor::Start},
  };

  text_engine_detail::applyTextAnchor(runs, chunks, text, /*vertical=*/false);

  EXPECT_THAT(chunks, ElementsAre(ChunkTextAnchorIs(TextAnchor::End)));
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty()),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(5.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(100.0))))));
}

TEST(TextEngineHelperTest, ApplyTextAnchorAdjustsVerticalMiddleChunks) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.yPosition = 0.0, .yAdvance = 10.0}, {.yPosition = 10.0, .yAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(1);
  text.spans[0].textAnchor = TextAnchor::Middle;
  std::vector<text_engine_detail::ChunkBoundary> chunks = {
      {.runIndex = 0, .glyphIndex = 0, .textAnchor = TextAnchor::Start},
  };

  text_engine_detail::applyTextAnchor(runs, chunks, text, /*vertical=*/true);

  EXPECT_THAT(chunks, ElementsAre(ChunkTextAnchorIs(TextAnchor::Middle)));
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(-10.0)),
                                                         GlyphYPositionIs(DoubleEq(0.0))))));
}

TEST(TextEngineHelperTest, ApplyTextAnchorLeavesEmptyEndChunksUnchanged) {
  std::vector<TextRun> runs = {
      {},
      {.glyphs = {{.xPosition = 20.0, .xAdvance = 10.0}}, .onPath = true},
  };
  components::ComputedTextComponent text;
  text.spans.resize(2);
  text.spans[1].textAnchor = TextAnchor::End;
  std::vector<text_engine_detail::ChunkBoundary> chunks = {
      {.runIndex = 0, .glyphIndex = 0, .textAnchor = TextAnchor::End},
  };

  text_engine_detail::applyTextAnchor(runs, chunks, text, /*vertical=*/false);

  EXPECT_THAT(chunks, ElementsAre(ChunkTextAnchorIs(TextAnchor::End)));
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty()),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(20.0))))));
}

TEST(TextEngineHelperTest, ComputeSpanBaselineShiftUsesFontMetricsAndLengthFallbacks) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  TextLayoutParams params = MakeTextParams(10.0);

  FakeTextBackend backendWithMetrics;
  backendWithMetrics.subSuperMetricsResult = SubSuperMetrics{
      .subscriptYOffset = 200,
      .superscriptYOffset = 300,
  };
  components::ComputedTextComponent::TextSpan span;
  span.baselineShiftKeyword = BSK::Sub;
  span.baselineShift = Lengthd(-0.33, Lengthd::Unit::Em);
  span.ancestorBaselineShifts.push_back({BSK::Super, Lengthd(0.4, Lengthd::Unit::Em), 20.0});
  span.ancestorBaselineShifts.push_back({BSK::Sub, Lengthd(-0.33, Lengthd::Unit::Em), 30.0});
  span.ancestorBaselineShifts.push_back({BSK::Length, Lengthd(3.0, Lengthd::Unit::None), 12.0});

  EXPECT_NEAR(text_engine_detail::computeSpanBaselineShiftPx(backendWithMetrics, span, FontHandle{},
                                                             0.01f, params),
              1.0, 1e-6);

  FakeTextBackend backendWithoutMetrics;
  components::ComputedTextComponent::TextSpan fallbackSpan;
  fallbackSpan.fontSize = Lengthd(20.0, Lengthd::Unit::Px);
  fallbackSpan.baselineShiftKeyword = BSK::Super;
  fallbackSpan.baselineShift = Lengthd(0.5, Lengthd::Unit::Em);
  fallbackSpan.ancestorBaselineShifts.push_back(
      {BSK::Sub, Lengthd(-0.33, Lengthd::Unit::Em), 20.0});

  EXPECT_NEAR(text_engine_detail::computeSpanBaselineShiftPx(backendWithoutMetrics, fallbackSpan,
                                                             FontHandle{}, 0.01f, params),
              3.4, 1e-6);
}

TEST(TextEngineTest, CachedGeometryApisFilterToRequestedSubtree) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = registry.create();
  const Entity child = registry.create();
  const Entity outside = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<donner::components::TreeComponent>(child, xml::XMLQualifiedNameRef("tspan"));
  registry.emplace<donner::components::TreeComponent>(outside, xml::XMLQualifiedNameRef("text"));
  registry.get<donner::components::TreeComponent>(root).appendChild(registry, child);
  registry.emplace<components::TextRootComponent>(root);

  components::ComputedTextGeometryComponent cache;
  cache.glyphs.push_back(components::ComputedTextGeometryComponent::GlyphGeometry{
      .sourceEntity = root,
      .path = PathBuilder().addRect(Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0)).build(),
      .extent = Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0),
  });
  cache.glyphs.push_back(components::ComputedTextGeometryComponent::GlyphGeometry{
      .sourceEntity = child,
      .path = PathBuilder().addRect(Box2d::FromXYWH(10.0, 0.0, 5.0, 5.0)).build(),
      .extent = Box2d::FromXYWH(10.0, 0.0, 5.0, 5.0),
  });
  cache.glyphs.push_back(components::ComputedTextGeometryComponent::GlyphGeometry{
      .sourceEntity = outside,
      .path = PathBuilder().addRect(Box2d::FromXYWH(100.0, 0.0, 5.0, 5.0)).build(),
      .extent = Box2d::FromXYWH(100.0, 0.0, 5.0, 5.0),
  });
  cache.characters.push_back(components::ComputedTextGeometryComponent::CharacterGeometry{
      .sourceEntity = root,
      .startPosition = Vector2d(0.0, 0.0),
      .endPosition = Vector2d(5.0, 0.0),
      .extent = Box2d::FromXYWH(0.0, 0.0, 5.0, 5.0),
      .rotation = 0.0,
      .advance = 5.0,
      .rendered = true,
      .hasExtent = true,
  });
  cache.characters.push_back(components::ComputedTextGeometryComponent::CharacterGeometry{
      .sourceEntity = child,
      .startPosition = Vector2d(10.0, 0.0),
      .endPosition = Vector2d(16.0, 0.0),
      .extent = Box2d::FromXYWH(10.0, 0.0, 6.0, 5.0),
      .rotation = 15.0,
      .advance = 6.0,
      .rendered = true,
      .hasExtent = true,
  });
  cache.characters.push_back(components::ComputedTextGeometryComponent::CharacterGeometry{
      .sourceEntity = outside,
      .startPosition = Vector2d(100.0, 0.0),
      .endPosition = Vector2d(108.0, 0.0),
      .extent = Box2d::FromXYWH(100.0, 0.0, 8.0, 5.0),
      .rotation = 30.0,
      .advance = 8.0,
      .rendered = true,
      .hasExtent = true,
  });
  cache.inkBounds = Box2d::FromXYWH(0.0, 0.0, 105.0, 5.0);
  cache.emBoxBounds = Box2d::FromXYWH(0.0, -10.0, 16.0, 20.0);
  registry.emplace<components::ComputedTextGeometryComponent>(root, std::move(cache));

  const EntityHandle rootHandle(registry, root);
  const EntityHandle childHandle(registry, child);
  EXPECT_EQ(engine.computedGlyphPaths(rootHandle).size(), 2u);
  EXPECT_EQ(engine.computedGlyphPaths(childHandle).size(), 1u);
  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 2);
  EXPECT_EQ(engine.getNumberOfChars(childHandle), 1);
  EXPECT_DOUBLE_EQ(engine.getComputedTextLength(rootHandle), 11.0);
  EXPECT_DOUBLE_EQ(engine.getSubStringLength(rootHandle, 1, 99), 6.0);
  EXPECT_EQ(engine.getStartPositionOfChar(childHandle, 0), Vector2d(10.0, 0.0));
  EXPECT_EQ(engine.getStartPositionOfChar(childHandle, 1), Vector2d());
  EXPECT_EQ(engine.getEndPositionOfChar(childHandle, 0), Vector2d(16.0, 0.0));
  EXPECT_EQ(engine.getEndPositionOfChar(childHandle, 1), Vector2d());
  EXPECT_EQ(engine.getExtentOfChar(childHandle, 0), Box2d::FromXYWH(10.0, 0.0, 6.0, 5.0));
  EXPECT_EQ(engine.getExtentOfChar(childHandle, 1), Box2d());
  EXPECT_DOUBLE_EQ(engine.getRotationOfChar(childHandle, 0), 15.0);
  EXPECT_DOUBLE_EQ(engine.getRotationOfChar(childHandle, 1), 0.0);
  EXPECT_EQ(engine.getCharNumAtPosition(rootHandle, Vector2d(12.0, 2.0)), 1);
  EXPECT_EQ(engine.getCharNumAtPosition(rootHandle, Vector2d(90.0, 2.0)), -1);
  EXPECT_EQ(engine.computedObjectBoundingBox(rootHandle), Box2d::FromXYWH(0.0, -10.0, 16.0, 20.0));
  EXPECT_EQ(engine.computedObjectBoundingBox(childHandle), Box2d::FromXYWH(10.0, 0.0, 5.0, 5.0));
}

TEST(TextEngineTest, ScriptedHorizontalLayoutCoversChunkAndSpanPositioningBranches) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span1 = MakeSpan("A");
  span1.xList = {Lengthd(10.0, Lengthd::Unit::None)};
  span1.yList = {Lengthd(20.0, Lengthd::Unit::None)};
  span1.dxList = {Lengthd(1.0, Lengthd::Unit::None)};
  span1.dyList = {Lengthd(2.0, Lengthd::Unit::None)};

  auto span2 = MakeSpan("B c");
  span2.startsNewChunk = false;
  span2.xList = {std::nullopt, std::nullopt, Lengthd(80.0, Lengthd::Unit::None)};
  span2.yList = {std::nullopt, std::nullopt, Lengthd(40.0, Lengthd::Unit::None)};
  span2.dxList = {std::nullopt, Lengthd(1.0, Lengthd::Unit::None),
                  Lengthd(2.0, Lengthd::Unit::None)};
  span2.dyList = {std::nullopt, Lengthd(3.0, Lengthd::Unit::None),
                  Lengthd(4.0, Lengthd::Unit::None)};
  span2.rotateList = {5.0, 15.0};
  span2.letterSpacingPx = 2.0;
  span2.wordSpacingPx = 5.0;

  text.spans.push_back(std::move(span1));
  text.spans.push_back(std::move(span2));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
                      AllOf(GlyphXPositionIs(DoubleEq(11.0)), GlyphYPositionIs(DoubleEq(22.0))))),
                  RunGlyphsAre(ElementsAre(
                      AllOf(GlyphXPositionIs(DoubleEq(24.0)), GlyphRotateDegreesIs(DoubleEq(5.0))),
                      AllOf(GlyphXPositionIs(DoubleEq(38.0)), GlyphRotateDegreesIs(DoubleEq(15.0))),
                      AllOf(GlyphXPositionIs(DoubleEq(82.0)), GlyphYPositionIs(DoubleEq(44.0)),
                            GlyphRotateDegreesIs(DoubleEq(15.0)))))));
}

TEST(TextEngineTest, ScriptedSupplementaryCoordinateListsUseLowSurrogateSlot) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  std::string textValue = "A";
  Utf8::Append(U'\U0001F600', std::back_inserter(textValue));
  textValue += "B";

  components::ComputedTextComponent text;
  auto span = MakeSpan(textValue);
  span.xList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt, std::nullopt,
                Lengthd(70.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt, std::nullopt,
                Lengthd(90.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(
                  GlyphXPositionIs(DoubleEq(0.0)), GlyphXPositionIs(DoubleEq(13.0)),
                  AllOf(GlyphXPositionIs(DoubleEq(70.0)), GlyphYPositionIs(DoubleEq(90.0)))))));
}

TEST(TextEngineTest, ScriptedVerticalLayoutCoversSidewaysUprightSpacingAndRotation) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  std::string textValue = "A";
  Utf8::Append(U'\u65E5', std::back_inserter(textValue));
  textValue += " ";

  components::ComputedTextComponent text;
  auto span = MakeSpan(textValue);
  span.xList = {Lengthd(100.0, Lengthd::Unit::None), Lengthd(110.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(10.0, Lengthd::Unit::None), Lengthd(40.0, Lengthd::Unit::None)};
  span.dxList = {Lengthd(1.0, Lengthd::Unit::None), Lengthd(2.0, Lengthd::Unit::None)};
  span.dyList = {Lengthd(3.0, Lengthd::Unit::None), Lengthd(4.0, Lengthd::Unit::None)};
  span.rotateList = {7.0, 17.0};
  span.letterSpacingPx = 2.0;
  span.wordSpacingPx = 5.0;
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        GlyphRotateDegreesIs(DoubleEq(97.0)),
                        AllOf(GlyphRotateDegreesIs(DoubleEq(17.0)),
                              GlyphXPositionIs(DoubleEq(114.0)), GlyphYPositionIs(DoubleEq(47.0))),
                        GlyphRotateDegreesIs(DoubleEq(107.0))))));
  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(3))));
  EXPECT_THAT(runs[0].glyphs[2].yPosition, Gt(runs[0].glyphs[1].yPosition));
}

TEST(TextEngineTest, ScriptedRtlChunkYOverrideKeepsVisualGlyphsOnSameBaseline) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager, /*reverseClusters=*/true);

  components::ComputedTextComponent text;
  auto span = MakeSpan("ABCD");
  span.xList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(50.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(60.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(_, _, GlyphYPositionIs(DoubleEq(60.0)),
                                                         GlyphYPositionIs(DoubleEq(60.0))))));
}

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

  EXPECT_THAT(runs,
              ElementsAre(AllOf(RunFontIs(amiri),
                                RunGlyphsAre(AllOf(Not(IsEmpty()), Each(GlyphIndexIs(Gt(0))))))));
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

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(AllOf(SizeIs(2), Each(GlyphIndexIs(Gt(0)))))));

  const float scale = engine.scaleForEmToPixels(runs[0].font, 64.0f);
  const Path firstGlyphPath =
      engine.glyphOutline(runs[0].font, runs[0].glyphs[0].glyphIndex, scale);
  ASSERT_THAT(firstGlyphPath.commands(), Not(IsEmpty()));

  Box2d positionedBounds = firstGlyphPath.bounds();
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

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(90.0, 1.0)),
                                                   GlyphYPositionIs(DoubleNear(130.0, 1.0)),
                                                   GlyphYPositionIs(DoubleNear(150.0, 1.0))))));
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
  ASSERT_THAT(unrotatedRuns, ElementsAre(RunGlyphsAre(SizeIs(2))));

  text.spans[0].rotateList = {30.0};
  const auto rotatedRuns = engine.layout(text, params);
  ASSERT_THAT(rotatedRuns, ElementsAre(RunGlyphsAre(SizeIs(2))));

  const auto& unrotatedBase = unrotatedRuns[0].glyphs[0];
  const auto& unrotatedMark = unrotatedRuns[0].glyphs[1];
  const auto& rotatedBase = rotatedRuns[0].glyphs[0];
  const auto& rotatedMark = rotatedRuns[0].glyphs[1];

  const double dx = unrotatedMark.xPosition - unrotatedBase.xPosition;
  const double dy = unrotatedMark.yPosition - unrotatedBase.yPosition;
  const double angle = 30.0 * MathConstants<double>::kDegToRad;
  const double expectedDx = dx * std::cos(angle) - dy * std::sin(angle);
  const double expectedDy = dx * std::sin(angle) + dy * std::cos(angle);

  EXPECT_THAT(rotatedRuns[0].glyphs, ElementsAre(GlyphRotateDegreesIs(DoubleEq(30.0)),
                                                 GlyphRotateDegreesIs(DoubleEq(30.0))));
  EXPECT_NEAR(rotatedMark.xPosition - rotatedBase.xPosition, expectedDx, 1.0);
  EXPECT_NEAR(rotatedMark.yPosition - rotatedBase.yPosition, expectedDy, 1.0);
}

TEST(TextEngineTest, TextPathTspanCoordinatesAffectPathLocalPlacement) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));

  components::ComputedTextComponent text;
  auto span1 = MakeSpan("Some ");
  auto span2 = MakeSpan("long");
  span2.startsNewChunk = false;
  auto span3 = MakeSpan(" text");
  span3.startsNewChunk = false;

  Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();

  span1.pathSpline = path;
  span1.textPathSourceEntity = Entity{1};
  span2.pathSpline = path;
  span2.textPathSourceEntity = Entity{1};
  span3.pathSpline = path;
  span3.textPathSourceEntity = Entity{1};

  span2.xList = {Lengthd(10.0, Lengthd::Unit::None)};
  span2.yList = {Lengthd(20.0, Lengthd::Unit::None)};
  span3.dxList = {Lengthd(5.0, Lengthd::Unit::None)};
  span3.dyList = {Lengthd(-10.0, Lengthd::Unit::None)};

  text.spans.push_back(std::move(span1));
  text.spans.push_back(std::move(span2));
  text.spans.push_back(std::move(span3));

  TextLayoutParams params = MakeTextParams(24.0);
  params.fontFamilies = {RcString("Noto Sans")};
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(Not(IsEmpty())), RunGlyphsAre(Not(IsEmpty())),
                                RunGlyphsAre(Not(IsEmpty()))));

  EXPECT_THAT(runs[1].glyphs, FirstGlyphMatches(AllOf(GlyphXPositionIs(Gt(5.0)),
                                                      GlyphYPositionIs(DoubleNear(0.0, 1.0)))));
  const double secondSpanFirstGlyphX = runs[1].glyphs.front().xPosition;
  EXPECT_THAT(runs[2].glyphs, FirstGlyphMatches(AllOf(GlyphXPositionIs(Gt(secondSpanFirstGlyphX)),
                                                      GlyphYPositionIs(DoubleNear(0.0, 1.0)))));
}

TEST(TextEngineTest, TextAfterTextPathStartsAfterLastVisiblePathGlyph) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));

  components::ComputedTextComponent text;
  auto pathSpan = MakeSpan("Some");
  auto trailingSpan = MakeSpan(" tail");
  trailingSpan.startsNewChunk = false;

  Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();
  pathSpan.pathSpline = path;
  pathSpan.textPathSourceEntity = Entity{1};

  text.spans.push_back(std::move(pathSpan));
  text.spans.push_back(std::move(trailingSpan));

  TextLayoutParams params = MakeTextParams(24.0);
  params.fontFamilies = {RcString("Noto Sans")};
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(Not(IsEmpty())), RunGlyphsAre(Not(IsEmpty()))));
  const double lastPathGlyphX = runs[0].glyphs.back().xPosition;
  EXPECT_THAT(runs[1].glyphs,
              FirstGlyphMatches(GlyphXPositionIs(AllOf(Gt(lastPathGlyphX), Lt(180.0)))));
}

// End-to-end (engine.layout) coverage of the SVG2 inline-size auto-flow path, exercising the
// property → TextLayoutParams::inlineSizePx → wrap dispatch, including line-height derived from
// the backend's vertical metrics. The ScriptedTextBackend gives each glyph a 10px advance and a
// 20px normal line-height at font-size 20 ((ascent 1000 - descent -200) * (20/1200) = 20).
namespace {

// Distinct, rounded glyph baseline Y values across all runs, sorted ascending.
std::vector<long> DistinctBaselines(const std::vector<TextRun>& runs) {
  std::vector<long> ys;
  for (const auto& run : runs) {
    for (const auto& g : run.glyphs) {
      ys.push_back(std::lround(g.yPosition));
    }
  }
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  return ys;
}

}  // namespace

TEST(TextEngineTest, InlineSizeUnsetKeepsSingleLine) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("aaa bbb ccc"));

  TextLayoutParams params = MakeTextParams(20.0);  // inlineSizePx defaults to 0 (no wrapping).
  const auto runs = engine.layout(text, params);

  EXPECT_THAT(DistinctBaselines(runs), SizeIs(1));
}

TEST(TextEngineTest, InlineSizeWrapsIntoStackedLines) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("aaa bbb ccc"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.inlineSizePx = 45.0;  // Each 3-glyph word (~32px) fits alone; two words do not.
  const auto runs = engine.layout(text, params);

  // Three words → three stacked baselines spaced by the 20px line-height.
  const auto baselines = DistinctBaselines(runs);
  ASSERT_THAT(baselines, SizeIs(3));
  EXPECT_EQ(baselines[1] - baselines[0], 20);
  EXPECT_EQ(baselines[2] - baselines[1], 20);
}

TEST(TextEngineTest, InlineSizeLargeMeasureDoesNotWrap) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("aaa bbb ccc"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.inlineSizePx = 1000.0;  // Everything fits on one line.
  const auto runs = engine.layout(text, params);

  EXPECT_THAT(DistinctBaselines(runs), SizeIs(1));
}

}  // namespace donner::svg
