#include "donner/svg/text/TextEngine.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <tuple>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/FontFace.h"
#include "donner/svg/SVG.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextBackendSimple.h"
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
using ::testing::Eq;
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
  source.trusted = true;

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
        .unitsPerEm = 1000,
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
  source.trusted = true;

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

TEST(TextEngineHelperTest, ByteMappingsAndChunkRangesUseCodepointIndices) {
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
    EXPECT_EQ(mappings.byteToCharIdx[offsets[i]], i) << "offset index " << i;
  }
  EXPECT_EQ(mappings.byteToCharIdx[offsets[cOffsetIndex]], cOffsetIndex);
  EXPECT_EQ(mappings.byteToCharIdx[offsets[emojiOffsetIndex]], emojiOffsetIndex);
  EXPECT_EQ(mappings.byteToCharIdx[offsets[dOffsetIndex]], dOffsetIndex);

  SmallVector<std::optional<Lengthd>, 1> xList;
  xList.resize(offsets.size());
  xList[cOffsetIndex] = Lengthd(10.0, Lengthd::Unit::None);
  xList[emojiOffsetIndex] = Lengthd(20.0, Lengthd::Unit::None);
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

std::string ZeroLengthPenRecord(const std::vector<TextGlyph>& glyphs) {
  std::ostringstream record;
  record << std::setprecision(17);
  for (const auto& glyph : glyphs) {
    record << "[" << glyph.xPosition << "," << glyph.xAdvance << "]";
  }
  return record.str();
}

void CheckParsedZeroLengthAnchor(bool full, TextAnchor anchor,
                                 const std::vector<TextGlyph>& expected) {
  std::ostringstream source;
  source << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 200 200' "
            "font-family='Noto Sans' font-size='48'><text id='t' x='20' y='100' "
            "textLength='0' text-anchor='"
         << anchor << "'>Text</text></svg>";
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source.str(), warnings);
  ASSERT_THAT(parsed, ParseResultIs(_));
  auto document = std::move(parsed).result();
  const auto target = document.querySelector("#t");
  ASSERT_THAT(target, testing::Optional(_));
  Registry& registry = document.registry();
  FontManager fonts(registry);
  const FontHandle font = LoadResvgFont(fonts, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_THAT(font, Not(testing::Eq(FontHandle{})));
  std::unique_ptr<TextBackend> backend;
  if (full) {
    backend = std::make_unique<TextBackendFull>(fonts, registry);
  } else {
    backend = std::make_unique<TextBackendSimple>(fonts, registry);
  }
  TextEngine engine(fonts, registry, std::move(backend));
  const auto handle = target->unsafeEntityHandle();
  engine.prepareForElement(handle, warnings);
  const auto& geometry = engine.ensureComputedTextGeometryComponent(handle);
  ASSERT_THAT(geometry.runs, ElementsAre(RunGlyphsAre(SizeIs(expected.size()))));
  EXPECT_THAT(geometry.runs[0].font, testing::Eq(font));
  EXPECT_THAT(geometry.runs[0].glyphs,
              Each(Field("font", &TextGlyph::font,
                         testing::AnyOf(testing::Eq(FontHandle{}), testing::Eq(font)))));
  for (size_t index = 0; index < expected.size(); ++index) {
    EXPECT_THAT(geometry.runs[0].glyphs[index].xPosition,
                DoubleNear(expected[index].xPosition, 1e-8));
    EXPECT_THAT(geometry.runs[0].glyphs[index].xAdvance,
                DoubleNear(expected[index].xAdvance, 1e-8));
  }
  std::ostringstream name;
  name << (full ? "full_" : "basic_") << anchor << "_parsed_pens_advances";
  testing::Test::RecordProperty(name.str(), ZeroLengthPenRecord(geometry.runs[0].glyphs));
}

TEST(TextEngineTest, ZeroTextLengthAnchorsNotoSansUsingCompressedPenExtents) {
  for (bool full : {false, true}) {
    SCOPED_TRACE(full);
    Registry registry;
    FontManager fonts(registry);
    const FontHandle font = LoadResvgFont(fonts, "NotoSans-Regular.ttf", "Noto Sans");
    ASSERT_THAT(font, Not(testing::Eq(FontHandle{})));
    std::unique_ptr<TextBackend> backend;
    if (full) {
      backend = std::make_unique<TextBackendFull>(fonts, registry);
    } else {
      backend = std::make_unique<TextBackendSimple>(fonts, registry);
    }
    TextEngine engine(fonts, registry, std::move(backend));
    components::ComputedTextComponent text;
    auto span = MakeSpan("Text");
    span.xList = {Lengthd(20)};
    span.yList = {Lengthd(100)};
    text.spans.push_back(span);
    auto params = MakeTextParams(48);
    params.fontFamilies = {RcString("Noto Sans")};
    const auto natural = engine.layout(text, params);
    ASSERT_THAT(natural, ElementsAre(RunGlyphsAre(SizeIs(4))));
    EXPECT_THAT(natural[0].font, testing::Eq(font));
    EXPECT_THAT(natural[0].glyphs.front().xPosition, DoubleNear(20, 1e-9));
    EXPECT_THAT(natural[0].glyphs,
                Each(Field("font", &TextGlyph::font,
                           testing::AnyOf(testing::Eq(FontHandle{}), testing::Eq(font)))));
    auto compressed = natural[0].glyphs;
    const double naturalLength = compressed.back().xPosition + compressed.back().xAdvance - 20;
    const double gap = -naturalLength / 3;
    double minimum = 20;
    double maximum = 20;
    for (size_t index = 0; index < compressed.size(); ++index) {
      auto& glyph = compressed[index];
      glyph.xPosition += gap * static_cast<double>(index);
      minimum = std::min({minimum, glyph.xPosition, glyph.xPosition + glyph.xAdvance});
      maximum = std::max({maximum, glyph.xPosition, glyph.xPosition + glyph.xAdvance});
    }
    EXPECT_THAT(compressed.back().xPosition + compressed.back().xAdvance, DoubleNear(20, 1e-9));
    EXPECT_THAT(minimum, DoubleNear(compressed.back().xPosition, 1e-9));
    EXPECT_THAT(20 - minimum, DoubleNear(compressed.back().xAdvance, 1e-9));
    const std::string prefix = full ? "full_" : "basic_";
    RecordProperty(prefix + "natural_pens_advances", ZeroLengthPenRecord(natural[0].glyphs));
    RecordProperty(prefix + "compressed_pens_advances", ZeroLengthPenRecord(compressed));
    text.spans[0].textLength = Lengthd(0);
    for (TextAnchor anchor : {TextAnchor::Start, TextAnchor::Middle, TextAnchor::End}) {
      SCOPED_TRACE(anchor);
      text.spans[0].textAnchor = anchor;
      params.textAnchor = anchor;
      const auto actual = engine.layout(text, params);
      ASSERT_THAT(actual, ElementsAre(RunGlyphsAre(SizeIs(4))));
      const double alignment = anchor == TextAnchor::Middle ? minimum / 2 + maximum / 2
                               : anchor == TextAnchor::End  ? maximum
                                                            : minimum;
      for (size_t index = 0; index < compressed.size(); ++index) {
        EXPECT_THAT(actual[0].glyphs[index].xPosition,
                    DoubleNear(compressed[index].xPosition + 20 - alignment, 1e-9));
        EXPECT_THAT(actual[0].glyphs[index].xAdvance, DoubleEq(compressed[index].xAdvance));
      }
      std::ostringstream name;
      name << prefix << anchor << "_pens_advances";
      RecordProperty(name.str(), ZeroLengthPenRecord(actual[0].glyphs));
      CheckParsedZeroLengthAnchor(full, anchor, actual[0].glyphs);
    }
  }
}

TEST(TextEngineHelperTest, ApplyTextLengthAcceptsZeroGlobalSpacing) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(1);
  TextLayoutParams params = MakeTextParams(10.0);
  params.textLength = Lengthd(0.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::Spacing;

  text_engine_detail::applyTextLength(runs, text, {}, params, /*vertical=*/false,
                                      /*currentPenX=*/30.0, /*currentPenY=*/0.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(10.0)),
                                                         GlyphXPositionIs(DoubleEq(0.0))))));
}

TEST(TextEngineHelperTest, ApplyPerSpanTextLengthAdvancesFollowingRunsUntilAbsoluteReset) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.xPosition = 10.0, .xAdvance = 10.0}, {.xPosition = 20.0, .xAdvance = 10.0}}},
      {.glyphs = {{.xPosition = 30.0, .xAdvance = 10.0}}},
      {.glyphs = {{.xPosition = 40.0, .xAdvance = 10.0, .cluster = 0},
                  {.xPosition = 100.0, .xAdvance = 10.0, .cluster = 1}}},
      {.glyphs = {{.xPosition = 110.0, .xAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(4);
  text.spans[0].textLength = Lengthd(40.0, Lengthd::Unit::None);
  text.spans[0].lengthAdjust = LengthAdjust::Spacing;
  text.spans[2].text = RcString("AB");
  text.spans[2].start = 0;
  text.spans[2].end = 2;
  text.spans[2].xList = {std::nullopt, Lengthd(100.0, Lengthd::Unit::None)};
  const std::vector<text_engine_detail::RunPenExtent> extents = {
      {.startX = 10.0, .endX = 30.0},
      {.startX = 30.0, .endX = 40.0},
      {.startX = 40.0, .endX = 110.0},
      {.startX = 110.0, .endX = 120.0},
  };

  text_engine_detail::applyTextLength(runs, text, extents, MakeTextParams(10.0),
                                      /*vertical=*/false, /*currentPenX=*/120.0,
                                      /*currentPenY=*/0.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(10.0)),
                                                         GlyphXPositionIs(DoubleEq(40.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(50.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(60.0)),
                                                         GlyphXPositionIs(DoubleEq(100.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(110.0))))));
}

TEST(TextEngineHelperTest, ApplyPerSpanTextLengthAdvancesVerticallyUntilAbsoluteReset) {
  std::vector<TextRun> runs = {
      {.glyphs = {{.yPosition = 10.0, .yAdvance = 10.0}, {.yPosition = 20.0, .yAdvance = 10.0}}},
      {.glyphs = {{.yPosition = 30.0, .yAdvance = 10.0}}},
      {.glyphs = {{.yPosition = 40.0, .yAdvance = 10.0, .cluster = 0},
                  {.yPosition = 100.0, .yAdvance = 10.0, .cluster = 1}}},
      {.glyphs = {{.yPosition = 110.0, .yAdvance = 10.0}}},
  };
  components::ComputedTextComponent text;
  text.spans.resize(4);
  text.spans[0].textLength = Lengthd(40.0, Lengthd::Unit::None);
  text.spans[0].lengthAdjust = LengthAdjust::Spacing;
  text.spans[2].text = RcString("AB");
  text.spans[2].start = 0;
  text.spans[2].end = 2;
  text.spans[2].yList = {std::nullopt, Lengthd(100.0, Lengthd::Unit::None)};
  const std::vector<text_engine_detail::RunPenExtent> extents = {
      {.startY = 10.0, .endY = 30.0},
      {.startY = 30.0, .endY = 40.0},
      {.startY = 40.0, .endY = 110.0},
      {.startY = 110.0, .endY = 120.0},
  };

  text_engine_detail::applyTextLength(runs, text, extents, MakeTextParams(10.0),
                                      /*vertical=*/true, /*currentPenX=*/0.0,
                                      /*currentPenY=*/120.0);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(10.0)),
                                                         GlyphYPositionIs(DoubleEq(40.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(50.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(60.0)),
                                                         GlyphYPositionIs(DoubleEq(100.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(110.0))))));
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
  cache.objectBoundsByEntity.emplace(child, Box2d::FromXYWH(10.0, 0.0, 5.0, 5.0));
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

TEST(TextEngineTest, ScriptedSupplementaryCoordinatesUseCodepointSlot) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  std::string textValue = "A";
  Utf8::Append(U'\U0001F600', std::back_inserter(textValue));
  textValue += "B";

  components::ComputedTextComponent text;
  auto span = MakeSpan(textValue);
  span.xList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(70.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(0.0, Lengthd::Unit::None), std::nullopt,
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

// Sideways glyphs sit on the central baseline: halfway between ascent and descent in font units,
// scaled by the em scale and not normalized to a 1em box. The scripted font has ascent 1000,
// descent -200 and 1000 units per em, so at 20px the alphabetic baseline sits
// (1000 - 200) / 2 * 20 / 1000 = 8px line-under (left) of the inline-axis position in both
// vertical writing modes. The em scale is a float, hence the small tolerance.
TEST(TextEngineTest, SidewaysVerticalGlyphCentersOnAscentDescentMidpoint) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  for (const WritingMode mode : {WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    SCOPED_TRACE(testing::Message() << "writingMode=" << mode);
    components::ComputedTextComponent text;
    auto span = MakeSpan("A");
    span.xList = {Lengthd(100.0, Lengthd::Unit::None)};
    span.yList = {Lengthd(10.0, Lengthd::Unit::None)};
    text.spans.push_back(std::move(span));

    TextLayoutParams params = MakeTextParams(20.0);
    params.writingMode = mode;
    const auto runs = engine.layout(text, params);

    EXPECT_THAT(runs,
                ElementsAre(RunGlyphsAre(ElementsAre(AllOf(GlyphRotateDegreesIs(DoubleEq(90.0)),
                                                           GlyphXPositionIs(DoubleNear(92.0, 1e-5)),
                                                           GlyphYPositionIs(DoubleEq(10.0)))))));
  }
}

// Lays out one span at (100, 100) with the full text backend, the given families and writing mode.
std::vector<TextRun> LayOutFull(Registry& registry, FontManager& fontManager, std::string_view utf8,
                                SmallVector<RcString, 1> families,
                                WritingMode mode = WritingMode::HorizontalTb) {
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));
  components::ComputedTextComponent text;
  auto span = MakeSpan(std::string(utf8));
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(20.0);
  params.fontFamilies = std::move(families);
  params.writingMode = mode;
  return engine.layout(text, params);
}

TEST(TextEngineTest, SmallCapsSynthesisDoesNotLeakAcrossSimpleShapingRequests) {
  Registry registry;
  FontManager manager(registry);
  const FontHandle font = LoadResvgFont(manager, "NotoSans-Regular.ttf", "CapsFont");
  ASSERT_TRUE(static_cast<bool>(font));
  auto backend = std::make_unique<TextBackendSimple>(manager, registry);
  TextEngine engine(manager, registry, std::move(backend));
  for (FontSynthesisSmallCaps policy :
       {FontSynthesisSmallCaps::Auto, FontSynthesisSmallCaps::None, FontSynthesisSmallCaps::Auto}) {
    components::ComputedTextComponent text;
    auto span = MakeSpan("aA");
    span.fontVariant = FontVariant::SmallCaps;
    span.fontSynthesisSmallCaps = policy;
    text.spans.push_back(std::move(span));
    auto params = MakeTextParams(40);
    params.fontFamilies = {RcString("CapsFont")};
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs.front().glyphs, SizeIs(2));
    EXPECT_FLOAT_EQ(runs.front().glyphs[0].fontSizeScale,
                    policy == FontSynthesisSmallCaps::None ? 1.0f : 0.8f);
    EXPECT_FLOAT_EQ(runs.front().glyphs[1].fontSizeScale, 1.0f);
    EXPECT_EQ(runs.front().glyphs[0].cluster, 0u);
    EXPECT_EQ(runs.front().glyphs[1].cluster, 1u);
  }
}

TEST(TextEngineTest, SmallCapsSynthesisNoneRetainsRealOpenTypeSmallCaps) {
  Registry registry;
  FontManager manager(registry);
  const FontHandle font = LoadResvgFont(manager, "SourceSansPro-Regular.ttf", "NativeCaps");
  ASSERT_TRUE(static_cast<bool>(font));
  auto backend = std::make_unique<TextBackendFull>(manager, registry);
  const TextBackendFull& probe = *backend;
  ASSERT_TRUE(probe.hasSmallCapsFeature(font));
  const auto native = probe.shapeRun(font, 40, "a", 0, 1, false, FontVariant::SmallCaps, false);
  const auto normal = probe.shapeRun(font, 40, "a", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(native.glyphs, SizeIs(1));
  ASSERT_THAT(normal.glyphs, SizeIs(1));
  ASSERT_NE(native.glyphs[0].glyphIndex, normal.glyphs[0].glyphIndex);
  TextEngine engine(manager, registry, std::move(backend));
  components::ComputedTextComponent text;
  auto span = MakeSpan("a");
  span.fontVariant = FontVariant::SmallCaps;
  span.fontSynthesisSmallCaps = FontSynthesisSmallCaps::None;
  text.spans.push_back(std::move(span));
  auto params = MakeTextParams(40);
  params.fontFamilies = {RcString("NativeCaps")};
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs.front().glyphs, SizeIs(1));
  EXPECT_EQ(runs.front().glyphs[0].glyphIndex, native.glyphs[0].glyphIndex);
  EXPECT_FLOAT_EQ(runs.front().glyphs[0].fontSizeScale, 1.0f);
}

// A fallback item is one part of its chunk: in a right-to-left chunk the neutral parentheses
// around an Arabic word that falls back to another face still take the chunk's direction, so they
// mirror, and the items keep visual order.
TEST(TextEngineTest, FallbackItemsInRightToLeftChunkMirrorNeutralsInVisualOrder) {
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "Amiri-Regular.ttf", "Amiri")));

  const auto glyphIndexOf = [&](std::string_view utf8) {
    const auto runs = LayOutFull(registry, fontManager, utf8, {RcString("Noto Sans")});
    return runs.empty() || runs[0].glyphs.empty() ? 0 : runs[0].glyphs[0].glyphIndex;
  };
  const int openParen = glyphIndexOf("(");
  const int closeParen = glyphIndexOf(")");
  ASSERT_THAT(openParen, Gt(0));
  ASSERT_THAT(closeParen, Gt(0));

  // "(" at byte 0, Arabic meem at 1, reh at 3, ")" at 5.
  const auto runs =
      LayOutFull(registry, fontManager, "(\xD9\x85\xD8\xB1)", {RcString("Noto Sans")});
  ASSERT_THAT(runs, SizeIs(1));
  std::vector<uint32_t> clusters;
  for (const TextGlyph& glyph : runs[0].glyphs) {
    clusters.push_back(glyph.cluster);
  }
  EXPECT_THAT(clusters, ElementsAre(5u, 3u, 1u, 0u));
  // Visually leftmost is the logical ")" drawn mirrored, rightmost the logical "(" mirrored.
  EXPECT_THAT(runs[0].glyphs, ElementsAre(GlyphIndexIs(openParen), _, _, GlyphIndexIs(closeParen)));
}

// Shaping each fallback item in its chunk's context must not rescan the whole chunk per item: a
// chunk that alternates faces on every character detects its context's direction and script once.
TEST(TextEngineTest, AlternatingFallbackChunkDetectsItsContextOnce) {
  for (const int repeats : {50, 200}) {
    SCOPED_TRACE(testing::Message() << "repeats=" << repeats);
    Registry registry;
    FontManager fontManager(registry);
    ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
    ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "Mplus 1p")));
    auto backend = std::make_unique<TextBackendFull>(fontManager, registry);
    const TextBackendFull& probe = *backend;
    TextEngine engine(fontManager, registry, std::move(backend));

    std::string text;
    for (int index = 0; index < repeats; ++index) {
      text += "a\xE6\x97\xA5";  // a, then a CJK character only the fallback face covers
    }
    components::ComputedTextComponent computed;
    computed.spans.push_back(MakeSpan(text));
    TextLayoutParams params = MakeTextParams(20.0);
    params.fontFamilies = {RcString("Noto Sans")};
    const auto runs = engine.layout(computed, params);
    ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(static_cast<size_t>(2 * repeats)))));
    EXPECT_THAT(probe.detectedContextBytesForTesting(), testing::Le(text.size()));
  }
}

// European digits in a right-to-left chunk keep their left-to-right order when they fall in a
// different face's item than the Arabic word.
TEST(TextEngineTest, DigitsInRightToLeftFallbackChunkKeepLeftToRightOrder) {
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "Amiri-Regular.ttf", "Amiri")));

  // Meem at byte 0, reh at 2, space at 4, "1" at 5, "2" at 6.
  const auto runs =
      LayOutFull(registry, fontManager, "\xD9\x85\xD8\xB1 12", {RcString("Noto Sans")});
  ASSERT_THAT(runs, SizeIs(1));
  std::optional<double> one;
  std::optional<double> two;
  for (const TextGlyph& glyph : runs[0].glyphs) {
    if (glyph.cluster == 5) {
      one = glyph.xPosition;
    } else if (glyph.cluster == 6) {
      two = glyph.xPosition;
    }
  }
  ASSERT_THAT(one, testing::Optional(_));
  ASSERT_THAT(two, testing::Optional(_));
  EXPECT_THAT(*one, Lt(*two));
}

// Per-character positions split right-to-left text into one-codepoint chunks, each shaped left to
// right on its own. A chunk that falls back whole to another face is shaped the same way as when
// that face is the span's own.
TEST(TextEngineTest, OneCodepointFallbackChunkShapesLikeThePrimaryFace) {
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "Amiri-Regular.ttf", "Amiri")));

  const auto layOut = [&](const char* family) {
    TextEngine engine(fontManager, registry,
                      std::make_unique<TextBackendFull>(fontManager, registry));
    components::ComputedTextComponent text;
    auto span = MakeSpan("\xD9\x87\xD8\xA7\xD9\x84\xD9\x88");  // four Arabic letters
    for (const double x : {10.0, 50.0, 90.0, 130.0}) {
      span.xList.push_back(Lengthd(x, Lengthd::Unit::None));
    }
    span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    text.spans.push_back(std::move(span));
    TextLayoutParams params = MakeTextParams(40.0);
    params.fontFamilies = {RcString(family)};
    std::vector<std::tuple<int, double, double>> glyphs;
    for (const TextRun& run : engine.layout(text, params)) {
      for (const TextGlyph& glyph : run.glyphs) {
        glyphs.emplace_back(glyph.glyphIndex, glyph.xPosition, glyph.xAdvance);
      }
    }
    return glyphs;
  };

  const auto primary = layOut("Amiri");
  ASSERT_THAT(primary, SizeIs(4));
  EXPECT_THAT(layOut("Noto Sans"), testing::ContainerEq(primary));
}

#ifdef DONNER_TEXT_FULL
struct NativeKanaPositioningCase {
  std::string name;
  std::string sourcePair;
  std::string mappedPair;
  std::string precomposed;
};

class FullWidthKanaNativeTest : public testing::TestWithParam<NativeKanaPositioningCase> {};

TEST_P(FullWidthKanaNativeTest, PositionedMarkPreservesNativeCompositionAndSourceAddressing) {
  const auto& input = GetParam();
  Registry registry;
  FontManager fonts(registry);
  const FontHandle font = LoadResvgFont(fonts, "MPLUS1p-Regular.ttf", "Kana Regression");
  ASSERT_THAT(font, Not(Eq(FontHandle())));
  TextBackendFull backend(fonts, registry);
  const auto pair = backend.shapeRun(font, 24, input.mappedPair, 0, input.mappedPair.size(), false,
                                     TextOrientation::Mixed, FontVariant::Normal, false);
  const auto precomposed =
      backend.shapeRun(font, 24, input.precomposed, 0, input.precomposed.size(), false,
                       TextOrientation::Mixed, FontVariant::Normal, false);
  ASSERT_THAT(precomposed.glyphs, SizeIs(1));
  ASSERT_THAT(pair.glyphs, ElementsAre(Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex,
                                             AllOf(Gt(0), Eq(precomposed.glyphs[0].glyphIndex)))));
  EXPECT_THAT(pair.glyphs[0].xAdvance, DoubleNear(precomposed.glyphs[0].xAdvance, 1e-5));

  components::ComputedTextComponent text;
  auto span = MakeSpan(input.sourcePair + "A");
  span.textTransform = TextTransform::FullWidth;
  span.xList = {Lengthd(0), std::nullopt, Lengthd(200)};
  text.spans.push_back(std::move(span));
  auto params = MakeTextParams(24);
  params.fontFamilies = {RcString("Kana Regression")};
  params.fontKerning = FontKerning::None;
  TextEngine engine(fonts, registry);
  const auto control = engine.layout(text, params);
  ASSERT_THAT(control, SizeIs(1));
  ASSERT_THAT(control[0].font, Eq(font));
  ASSERT_THAT(control[0].glyphs, SizeIs(2));
  ASSERT_THAT(control[0].glyphs[0].glyphIndex, Eq(pair.glyphs[0].glyphIndex));

  text.spans[0].xList[1] = Lengthd(100);
  const auto positioned = engine.layout(text, params);
  ASSERT_THAT(positioned, SizeIs(1));
  ASSERT_THAT(positioned[0].font, Eq(font));
  ASSERT_THAT(positioned[0].glyphs, ElementsAre(GlyphIndexIs(Eq(control[0].glyphs[0].glyphIndex)),
                                                GlyphIndexIs(Eq(control[0].glyphs[1].glyphIndex))));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_THAT(positioned[0].glyphs[i].xPosition,
                DoubleNear(control[0].glyphs[i].xPosition, 1e-5));
    EXPECT_THAT(positioned[0].glyphs[i].xAdvance, DoubleNear(control[0].glyphs[i].xAdvance, 1e-5));
    EXPECT_THAT(GlyphFont(positioned[0], positioned[0].glyphs[i]), Eq(font));
  }
  EXPECT_THAT(positioned[0].glyphs, ElementsAre(Field("cluster", &TextGlyph::cluster, 0u),
                                                Field("cluster", &TextGlyph::cluster, 6u)));
  EXPECT_THAT(text.spans[0].text, Eq(RcString(input.sourcePair + "A")));
}

INSTANTIATE_TEST_SUITE_P(
    Positioned, FullWidthKanaNativeTest,
    testing::Values(NativeKanaPositioningCase{"Voiced", "\xef\xbd\xb6\xef\xbe\x9e",
                                              "\xe3\x82\xab\xe3\x82\x99", "\xe3\x82\xac"},
                    NativeKanaPositioningCase{"Semivoiced", "\xef\xbe\x8a\xef\xbe\x9f",
                                              "\xe3\x83\x8f\xe3\x82\x9a", "\xe3\x83\x91"}),
    [](const testing::TestParamInfo<NativeKanaPositioningCase>& info) { return info.param.name; });
#endif

// A range shaped in context joins with its neighbors: the middle seen of three takes the same
// medial glyph as when the whole word is shaped at once, not its isolated form.
TEST(TextEngineTest, RangeShapedInContextJoinsWithItsNeighbors) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle amiri = LoadResvgFont(fontManager, "Amiri-Regular.ttf", "Amiri");
  ASSERT_TRUE(static_cast<bool>(amiri));
  TextBackendFull backend(fontManager, registry);

  const std::string word = "\xD8\xB3\xD8\xB3\xD8\xB3";  // seen seen seen, at bytes 0, 2 and 4
  const auto whole = backend.shapeRun(amiri, 40.0f, word, 0, word.size(), false,
                                      TextOrientation::Mixed, FontVariant::Normal, false);
  int wholeMiddle = 0;
  for (const auto& glyph : whole.glyphs) {
    if (glyph.cluster == 2) {
      wholeMiddle = glyph.glyphIndex;
    }
  }
  ASSERT_THAT(wholeMiddle, Gt(0));

  const TextBackend::ShapingContext context{0, word.size()};
  const auto middle = backend.shapeRunInContext(amiri, 40.0f, word, context, 2, 2, false,
                                                TextOrientation::Mixed, FontVariant::Normal, true);
  const auto alone = backend.shapeRun(amiri, 40.0f, word, 2, 2, false, TextOrientation::Mixed,
                                      FontVariant::Normal, false);
  ASSERT_THAT(alone.glyphs, SizeIs(1));
  ASSERT_THAT(alone.glyphs[0].glyphIndex, Not(wholeMiddle));  // the isolated form differs
  EXPECT_THAT(middle.glyphs,
              ElementsAre(Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, wholeMiddle)));
}

// A fallback item inside a vertical chunk that contains upright CJK is shaped the way its chunk
// is: vertically, with sideways Latin advanced by its unkerned horizontal advance.
TEST(TextEngineTest, FallbackLatinItemInVerticalChunkUsesTheChunkVerticalShaping) {
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoEmoji-Regular.ttf", "Noto Emoji")));
  const FontHandle yellowtail = LoadResvgFont(fontManager, "Yellowtail-Regular.ttf", "Yellowtail");
  ASSERT_TRUE(static_cast<bool>(yellowtail));
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "Mplus 1p")));

  // Noto Emoji has neither; Yellowtail has the Latin but no CJK, so the chunk splits in two items.
  const std::string text =
      "\xE6\x97\xA5\xE6\x9C\xAC"
      "AVATAR";
  const auto runs =
      LayOutFull(registry, fontManager, text,
                 {RcString("Noto Emoji"), RcString("Yellowtail"), RcString("Mplus 1p")},
                 WritingMode::VerticalRl);
  ASSERT_THAT(runs, SizeIs(1));

  TextBackendFull backend(fontManager, registry);
  const auto oracle = backend.shapeRun(yellowtail, runs[0].usedFontSizePx, text, 0, text.size(),
                                       true, TextOrientation::Mixed, FontVariant::Normal, false);
  std::vector<double> expected;
  for (const auto& glyph : oracle.glyphs) {
    if (glyph.cluster >= 6) {
      expected.push_back(glyph.xAdvance);
    }
  }
  std::vector<double> actual;
  for (const TextGlyph& glyph : runs[0].glyphs) {
    if (glyph.cluster >= 6) {
      EXPECT_THAT(glyph.font, testing::Eq(yellowtail));
      actual.push_back(glyph.yAdvance);
    }
  }
  EXPECT_THAT(actual, testing::Pointwise(DoubleNear(1e-9), expected));
}

// A vertical chunk's anchor length runs from the pen position before its first glyph to the pen
// position after its last. Upright CJK glyphs sit below their pen position by the face's vertical
// origin while sideways Latin glyphs sit on it, so a chunk mixing them must not measure from the
// first glyph's drawn position. Middle anchoring shifts every glyph of the start-anchored layout
// back by half the pen extent; with the chunk starting at y=0 and ending on a sideways glyph, that
// extent is the last glyph's end.
TEST(TextEngineTest, VerticalMiddleAnchorCentersPenExtentOfMixedOrientationChunk) {
  for (const bool fullBackend : {false, true}) {
    SCOPED_TRACE(fullBackend ? "full backend" : "simple backend");
    Registry registry;
    FontManager fontManager(registry);
    ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "Mplus 1p")));
    std::unique_ptr<TextBackend> backend;
    if (fullBackend) {
      backend = std::make_unique<TextBackendFull>(fontManager, registry);
    } else {
      backend = std::make_unique<TextBackendSimple>(fontManager, registry);
    }
    TextEngine engine(fontManager, registry, std::move(backend));

    const auto layOut = [&](TextAnchor anchor) {
      components::ComputedTextComponent text;
      auto span = MakeSpan("\xE6\x97\xA5\xE6\x9C\xAC Ab");  // 日本 Ab
      span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
      span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
      span.textAnchor = anchor;
      text.spans.push_back(std::move(span));
      TextLayoutParams params = MakeTextParams(20.0);
      params.fontFamilies = {RcString("Mplus 1p")};
      params.writingMode = WritingMode::VerticalRl;
      return engine.layout(text, params);
    };

    const std::vector<TextRun> start = layOut(TextAnchor::Start);
    ASSERT_THAT(start, ElementsAre(RunGlyphsAre(SizeIs(5))));
    // The first glyph is upright and the last sideways at its pen position. The full backend draws
    // the upright glyph below its pen by the face's vertical origin; the simple backend's origin
    // for this face coincides with the pen, so only the full backend exercises the offset.
    ASSERT_THAT(start[0].glyphs.front(), GlyphRotateDegreesIs(DoubleEq(0.0)));
    ASSERT_THAT(start[0].glyphs.back(), GlyphRotateDegreesIs(DoubleEq(90.0)));
    if (fullBackend) {
      ASSERT_THAT(start[0].glyphs.front(), GlyphYPositionIs(Gt(0.0)));
    }
    const double penExtent = start[0].glyphs.back().yPosition + start[0].glyphs.back().yAdvance;

    std::vector<double> expected;
    for (const TextGlyph& glyph : start[0].glyphs) {
      expected.push_back(glyph.yPosition - penExtent / 2.0);
    }
    const std::vector<TextRun> middle = layOut(TextAnchor::Middle);
    ASSERT_THAT(middle, SizeIs(1));
    std::vector<double> actual;
    for (const TextGlyph& glyph : middle[0].glyphs) {
      actual.push_back(glyph.yPosition);
    }
    EXPECT_THAT(actual, testing::Pointwise(DoubleNear(1e-9), expected));
  }
}

// Further vertical anchor cases in the full backend: an end anchor, a chunk that starts sideways
// and ends upright, and an all-upright control whose glyph offsets cancel. Each anchored layout is
// the start-anchored layout shifted back by the anchor's share of the pen extent, which runs from
// y=0 to the last glyph's pen position plus its advance.
TEST(TextEngineTest, VerticalAnchorsMeasurePenExtentForEndSidewaysFirstAndUprightChunks) {
  struct AnchorCase {
    const char* label;
    std::string text;
    TextAnchor anchor;
    double share;
  };
  const AnchorCase cases[] = {
      {"end, upright first", "\xE6\x97\xA5\xE6\x9C\xAC Ab", TextAnchor::End, 1.0},
      {"middle, sideways first", "Ab \xE6\x97\xA5\xE6\x9C\xAC", TextAnchor::Middle, 0.5},
      {"middle, all upright", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", TextAnchor::Middle, 0.5},
  };
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "Mplus 1p")));
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));

  for (const AnchorCase& testCase : cases) {
    SCOPED_TRACE(testCase.label);
    const auto layOut = [&](TextAnchor anchor) {
      components::ComputedTextComponent text;
      auto span = MakeSpan(testCase.text);
      span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
      span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
      span.textAnchor = anchor;
      text.spans.push_back(std::move(span));
      TextLayoutParams params = MakeTextParams(20.0);
      params.fontFamilies = {RcString("Mplus 1p")};
      params.writingMode = WritingMode::VerticalRl;
      return engine.layout(text, params);
    };

    const std::vector<TextRun> start = layOut(TextAnchor::Start);
    ASSERT_THAT(start, SizeIs(1));
    ASSERT_THAT(start[0].glyphs, Not(IsEmpty()));
    const TextGlyph& last = start[0].glyphs.back();
    const double penExtent = last.yPosition - last.verticalOriginOffset + last.yAdvance;

    std::vector<double> expected;
    for (const TextGlyph& glyph : start[0].glyphs) {
      expected.push_back(glyph.yPosition - penExtent * testCase.share);
    }
    const std::vector<TextRun> anchored = layOut(testCase.anchor);
    ASSERT_THAT(anchored, SizeIs(1));
    std::vector<double> actual;
    for (const TextGlyph& glyph : anchored[0].glyphs) {
      actual.push_back(glyph.yPosition);
    }
    EXPECT_THAT(actual, testing::Pointwise(DoubleNear(1e-9), expected));
  }

  // The all-upright control checks the same length from drawn positions alone: the first and last
  // glyphs carry the same origin offset, which cancels.
  const auto uprightStart = [&] {
    components::ComputedTextComponent text;
    auto span = MakeSpan(cases[2].text);
    span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
    text.spans.push_back(std::move(span));
    TextLayoutParams params = MakeTextParams(20.0);
    params.fontFamilies = {RcString("Mplus 1p")};
    params.writingMode = WritingMode::VerticalRl;
    return engine.layout(text, params);
  }();
  ASSERT_THAT(uprightStart, ElementsAre(RunGlyphsAre(SizeIs(3))));
  const auto& upright = uprightStart[0].glyphs;
  EXPECT_THAT(upright.back().yPosition + upright.back().yAdvance - upright.front().yPosition,
              DoubleNear(60.0, 1e-9));
}

// Vertical spacingAndGlyphs stretches upright glyphs along the inline axis, so the distance from
// each glyph's pen position to its drawn origin stretches with it. With the chunk starting at
// y=0, every inline coordinate of the stretched layout is the natural one times the factor.
TEST(TextEngineTest, VerticalSpacingAndGlyphsScalesUprightGlyphsFromTheirPenPositions) {
  Registry registry;
  FontManager fontManager(registry);
  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "Mplus 1p")));
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));

  constexpr double kScale = 2.0;
  const auto layOut = [&](std::optional<double> spanLength, std::optional<double> elementLength) {
    components::ComputedTextComponent text;
    auto span = MakeSpan("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");  // three upright CJK glyphs
    span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(0.0, Lengthd::Unit::None));
    if (spanLength) {
      span.textLength = Lengthd(*spanLength, Lengthd::Unit::None);
      span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
    }
    text.spans.push_back(std::move(span));
    TextLayoutParams params = MakeTextParams(20.0);
    params.fontFamilies = {RcString("Mplus 1p")};
    params.writingMode = WritingMode::VerticalRl;
    if (elementLength) {
      params.textLength = Lengthd(*elementLength, Lengthd::Unit::None);
      params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
    }
    return engine.layout(text, params);
  };

  const std::vector<TextRun> natural = layOut(std::nullopt, std::nullopt);
  ASSERT_THAT(natural, ElementsAre(RunGlyphsAre(SizeIs(3))));
  // The upright glyphs sit below their pen positions, so a scale about the first drawn origin
  // would differ from a scale about the pen.
  ASSERT_THAT(natural[0].glyphs.front(), GlyphYPositionIs(Gt(0.0)));
  const double naturalLength = natural[0].glyphs.back().yPosition +
                               natural[0].glyphs.back().yAdvance -
                               natural[0].glyphs.front().yPosition;
  std::vector<double> expected;
  for (const TextGlyph& glyph : natural[0].glyphs) {
    expected.push_back(glyph.yPosition * kScale);
  }

  for (const bool onSpan : {true, false}) {
    SCOPED_TRACE(onSpan ? "span textLength" : "element textLength");
    const std::optional<double> length = naturalLength * kScale;
    const std::vector<TextRun> stretched =
        onSpan ? layOut(length, std::nullopt) : layOut(std::nullopt, length);
    ASSERT_THAT(stretched, ElementsAre(RunGlyphsAre(SizeIs(3))));
    std::vector<double> actual;
    for (const TextGlyph& glyph : stretched[0].glyphs) {
      actual.push_back(glyph.yPosition);
    }
    EXPECT_THAT(actual, testing::Pointwise(DoubleNear(1e-6), expected));
    EXPECT_THAT(stretched[0].glyphs,
                Each(GlyphStretchScaleYIs(FloatEq(static_cast<float>(kScale)))));
  }
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

  const FontHandle notoSans = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(notoSans));
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

  // The run keeps the span's face; each Arabic glyph falls back to the face that covers it.
  EXPECT_THAT(runs, ElementsAre(AllOf(
                        RunFontIs(notoSans),
                        RunGlyphsAre(AllOf(Not(IsEmpty()),
                                           Each(AllOf(GlyphIndexIs(Gt(0)),
                                                      Field("font", &TextGlyph::font, amiri))))))));
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

TEST(TextEngineTest, SupplementaryCodepointPositionsPreserveEmojiClusterShaping) {
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
                Lengthd(130.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(32.0);
  params.fontFamilies = {RcString("Noto Color Emoji")};
  const auto runs = engine.layout(text, params);

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(90.0, 1.0)),
                                                   GlyphYPositionIs(DoubleNear(111.0, 1.0)),
                                                   GlyphYPositionIs(DoubleNear(130.0, 1.0))))));
}

#ifdef DONNER_TEXT_FULL
TEST(TextEngineTest, JoinedShapingKeepsPositioningSlotsAndRelativeCarry) {
  struct Case {
    const char* file;
    const char* family;
    const char* prefix;
    const char* following;
    bool bitmapOnly;
  };
  const Case cases[] = {
      {"NotoSansDevanagari-Regular.ttf", "Noto Sans Devanagari",
       "\xE0\xA4\x95\xE2\x80\x8D\xE0\xA4\xBF", "B", false},
      {"NotoColorEmoji.ttf", "Noto Color Emoji", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB",
       "\xF0\x9F\x98\x80", true},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.family);
    Registry registry;
    FontManager manager(registry);
    const FontHandle font = LoadResvgFont(manager, test.file, test.family);
    ASSERT_TRUE(static_cast<bool>(font));
    TextEngine engine(manager, registry);
    TextBackendFull backend(manager, registry);
    const std::string prefix(test.prefix);
    const auto shaped =
        backend.shapeRun(font, 20, prefix, 0, prefix.size(), false, FontVariant::Normal, false);
    ASSERT_FALSE(shaped.glyphs.empty());
    for (const auto& glyph : shaped.glyphs) {
      EXPECT_EQ(glyph.cluster, 0u);
    }
    components::ComputedTextComponent text;
    auto first = MakeSpan(prefix);
    first.xList = {Lengthd(15), Lengthd(99), Lengthd(99)};
    first.yList = {Lengthd(80), Lengthd(99), Lengthd(99)};
    first.dxList = {Lengthd(1), Lengthd(2), Lengthd(3)};
    first.dyList = {Lengthd(1), Lengthd(2), Lengthd(3)};
    first.rotateList = {0, 10, 20, 30};
    text.spans.push_back(std::move(first));
    auto following = MakeSpan(test.following);
    following.xList = {Lengthd(85)};
    following.yList = {Lengthd(130)};
    following.dxList = {Lengthd(4)};
    following.dyList = {Lengthd(4)};
    following.rotateList = {30};
    text.spans.push_back(std::move(following));
    auto params = MakeTextParams(20);
    params.fontFamilies = {RcString(test.family)};
    const auto runs = engine.layout(text, params);
    ASSERT_EQ(runs.size(), 2u);
    ASSERT_EQ(runs[0].glyphs.size(), shaped.glyphs.size());
    ASSERT_EQ(runs[1].glyphs.size(), 1u);
    EXPECT_THAT(runs[0].glyphs.front(),
                AllOf(GlyphXPositionIs(DoubleEq(16)), GlyphYPositionIs(DoubleEq(81)),
                      GlyphRotateDegreesIs(DoubleEq(0))));
    EXPECT_THAT(runs[1].glyphs.front(),
                AllOf(GlyphXPositionIs(DoubleEq(94)), GlyphYPositionIs(DoubleEq(139)),
                      GlyphRotateDegreesIs(DoubleEq(30))));
    double shapedAdvance = 0;
    for (size_t index = 0; index < shaped.glyphs.size(); ++index) {
      EXPECT_THAT(
          runs[0].glyphs[index],
          AllOf(GlyphXPositionIs(DoubleEq(16 + shapedAdvance + shaped.glyphs[index].xOffset)),
                GlyphYPositionIs(DoubleEq(81 + shaped.glyphs[index].yOffset)),
                GlyphRotateDegreesIs(DoubleEq(0))));
      shapedAdvance += shaped.glyphs[index].xAdvance;
    }
    size_t visibleGlyphs = 0;
    size_t invisibleGlyphs = 0;
    for (const auto& run : runs) {
      EXPECT_EQ(run.font, font);
      EXPECT_EQ(engine.isBitmapOnly(run.font), test.bitmapOnly);
      const float scale = engine.scaleForEmToPixels(run.font, run.usedFontSizePx);
      for (const auto& glyph : run.glyphs) {
        EXPECT_EQ(glyph.cluster, 0u);
        const Path outline = engine.glyphOutline(run.font, glyph.glyphIndex, scale);
        const auto bitmap = engine.bitmapGlyph(run.font, glyph.glyphIndex, scale);
        if (test.bitmapOnly) {
          EXPECT_TRUE(outline.empty());
          ASSERT_TRUE(bitmap.has_value());
          EXPECT_GT(bitmap->width, 0);
          EXPECT_GT(bitmap->height, 0);
          EXPECT_EQ(bitmap->rgbaPixels.size(),
                    static_cast<size_t>(bitmap->width) * bitmap->height * 4u);
          bool hasInk = false;
          for (size_t alpha = 3; alpha < bitmap->rgbaPixels.size(); alpha += 4) {
            hasInk |= bitmap->rgbaPixels[alpha] != 0;
          }
          EXPECT_TRUE(hasInk);
          ++visibleGlyphs;
        } else if (!outline.empty()) {
          ++visibleGlyphs;
        } else {
          EXPECT_EQ(glyph.xAdvance, 0);
          ++invisibleGlyphs;
        }
      }
    }
    EXPECT_GE(visibleGlyphs, test.bitmapOnly ? 2u : 3u);
    if (!test.bitmapOnly) {
      EXPECT_EQ(invisibleGlyphs, 1u);
    }
  }
}
#endif

TEST(TextEngineTest, CombiningMarkAfterSpaceAppliesWordSpacingOnce) {
  for (bool fullBackend : {false, true}) {
    SCOPED_TRACE(fullBackend ? "full backend" : "simple backend");
    Registry registry;
    FontManager fontManager(registry);
    const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
    ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
    std::unique_ptr<TextBackend> backend;
    if (fullBackend) {
      backend = std::make_unique<TextBackendFull>(fontManager, registry);
    } else {
      backend = std::make_unique<TextBackendSimple>(fontManager, registry);
    }
    const std::string value =
        "A \xCC\x81"
        "B";
    const auto shaped =
        backend->shapeRun(font, 24.0f, value, 0, value.size(), false, FontVariant::Normal, false);
    ASSERT_THAT(shaped.glyphs, SizeIs(4));
    ASSERT_THAT(shaped.glyphs[2].xAdvance, DoubleEq(0.0));
    TextEngine engine(fontManager, registry, std::move(backend));

    components::ComputedTextComponent text;
    text.spans.push_back(MakeSpan(value));
    TextLayoutParams params = MakeTextParams(24.0);
    params.fontFamilies = {RcString("Noto Sans")};
    const auto ordinary = engine.layout(text, params);
    ASSERT_THAT(ordinary, ElementsAre(RunGlyphsAre(SizeIs(4))));

    text.spans[0].wordSpacingPx = 11.0;
    const auto spaced = engine.layout(text, params);
    ASSERT_THAT(spaced, ElementsAre(RunGlyphsAre(SizeIs(4))));
    EXPECT_THAT(spaced[0].glyphs[2],
                GlyphXPositionIs(DoubleNear(ordinary[0].glyphs[2].xPosition, 1e-6)));
    EXPECT_THAT(spaced[0].glyphs.back(),
                GlyphXPositionIs(DoubleNear(ordinary[0].glyphs.back().xPosition + 11.0, 1e-6)));
    EXPECT_THAT(spaced[0].glyphs[2],
                Field("cluster", &TextGlyph::cluster, shaped.glyphs[2].cluster));
  }
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

TEST(TextEngineTest, VerticalSidewaysCombiningMarkMatchesRotatedHorizontalGeometry) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "Combining-mark positioning requires the full text backend.";
#endif

  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));

  components::ComputedTextComponent text;
  auto span = MakeSpan("x\xCC\x81");
  span.xList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  span.yList.push_back(Lengthd(100.0, Lengthd::Unit::None));
  text.spans.push_back(std::move(span));

  TextLayoutParams horizontalParams = MakeTextParams(64.0);
  horizontalParams.fontFamilies = {RcString("Noto Sans")};
  const auto horizontalRuns = engine.layout(text, horizontalParams);
  ASSERT_THAT(horizontalRuns, ElementsAre(RunGlyphsAre(SizeIs(2))));

  TextLayoutParams verticalParams = horizontalParams;
  verticalParams.writingMode = WritingMode::VerticalRl;
  const auto verticalRuns = engine.layout(text, verticalParams);
  ASSERT_THAT(verticalRuns, ElementsAre(RunGlyphsAre(SizeIs(2))));

  const Vector2d horizontalDelta(
      horizontalRuns[0].glyphs[1].xPosition - horizontalRuns[0].glyphs[0].xPosition,
      horizontalRuns[0].glyphs[1].yPosition - horizontalRuns[0].glyphs[0].yPosition);
  const Vector2d verticalDelta(
      verticalRuns[0].glyphs[1].xPosition - verticalRuns[0].glyphs[0].xPosition,
      verticalRuns[0].glyphs[1].yPosition - verticalRuns[0].glyphs[0].yPosition);

  EXPECT_THAT(verticalDelta.x, DoubleNear(-horizontalDelta.y, 0.01));
  EXPECT_THAT(verticalDelta.y, DoubleNear(horizontalDelta.x, 0.01));
}

TEST(TextEngineTest, UprightLatinUsesVerticalShapingWithoutAdvancingCombiningMarks) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "Vertical HarfBuzz shaping requires the full text backend.";
#endif

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  TextBackendFull backend(fontManager, registry);
  const std::string value =
      "x\xCC\x81"
      "B";

  const auto upright = backend.shapeRun(font, 32.0f, value, 0, value.size(), true,
                                        TextOrientation::Upright, FontVariant::Normal, false);
  ASSERT_THAT(upright.glyphs, SizeIs(3));
  EXPECT_THAT(upright.glyphs[0].xAdvance, DoubleEq(0.0));
  EXPECT_THAT(upright.glyphs[0].yAdvance, Gt(0.0));
  EXPECT_THAT(upright.glyphs[1].xAdvance, DoubleEq(0.0));
  EXPECT_THAT(upright.glyphs[1].yAdvance, DoubleEq(0.0));
  EXPECT_THAT(upright.glyphs[2].xAdvance, DoubleEq(0.0));
  EXPECT_THAT(upright.glyphs[2].yAdvance, Gt(0.0));

  components::ComputedTextComponent text;
  auto span = MakeSpan(value);
  span.textOrientation = TextOrientation::Upright;
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(32.0);
  params.fontFamilies = {RcString("Noto Sans")};
  params.writingMode = WritingMode::VerticalRl;
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(3))));
  const Vector2d expectedMarkFromBase(
      upright.glyphs[1].xOffset - upright.glyphs[0].xOffset,
      upright.glyphs[0].yAdvance + upright.glyphs[1].yOffset - upright.glyphs[0].yOffset);
  EXPECT_THAT(runs[0].glyphs[1].xPosition - runs[0].glyphs[0].xPosition,
              DoubleNear(expectedMarkFromBase.x, 1e-6));
  EXPECT_THAT(runs[0].glyphs[1].yPosition - runs[0].glyphs[0].yPosition,
              DoubleNear(expectedMarkFromBase.y, 1e-6));
  EXPECT_THAT(runs[0].glyphs[1].yAdvance, DoubleEq(0.0));
  const double basePenY = runs[0].glyphs[0].yPosition - upright.glyphs[0].yOffset;
  const double nextPenY = runs[0].glyphs[2].yPosition - upright.glyphs[2].yOffset;
  EXPECT_THAT(nextPenY, DoubleNear(basePenY + upright.glyphs[0].yAdvance, 1e-6));

  const float outlineScale = engine.scaleForEmToPixels(runs[0].font, 32.0f);
  Box2d baseBounds =
      engine.glyphOutline(runs[0].font, runs[0].glyphs[0].glyphIndex, outlineScale).bounds();
  Box2d markBounds =
      engine.glyphOutline(runs[0].font, runs[0].glyphs[1].glyphIndex, outlineScale).bounds();
  baseBounds += Vector2d(runs[0].glyphs[0].xPosition, runs[0].glyphs[0].yPosition);
  markBounds += Vector2d(runs[0].glyphs[1].xPosition, runs[0].glyphs[1].yPosition);
  EXPECT_THAT(markBounds.bottomRight.y, Lt(baseBounds.bottomRight.y));
  EXPECT_THAT(markBounds.bottomRight.x, Gt(baseBounds.topLeft.x));
  EXPECT_THAT(markBounds.topLeft.x, Lt(baseBounds.bottomRight.x));
}

TEST(TextEngineTest, SimpleUprightCombiningMarkStaysOnBaseCell) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  TextBackendSimple backend(fontManager, registry);
  const std::string value =
      "x\xCC\x81"
      "B";
  const auto shaped = backend.shapeRun(font, 32.0f, value, 0, value.size(), true,
                                       TextOrientation::Upright, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(3));
  ASSERT_THAT(shaped.glyphs[1].yAdvance, DoubleEq(0.0));

  components::ComputedTextComponent text;
  auto span = MakeSpan(value);
  span.textOrientation = TextOrientation::Upright;
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(32.0);
  params.fontFamilies = {RcString("Noto Sans")};
  params.writingMode = WritingMode::VerticalRl;
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendSimple>(fontManager, registry));
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(3))));
  EXPECT_THAT(runs[0].glyphs[1].yAdvance, DoubleEq(0.0));
  EXPECT_THAT(runs[0].glyphs[1].yPosition, DoubleNear(runs[0].glyphs[0].yPosition, 1e-6));
  const double basePenY = runs[0].glyphs[0].yPosition - shaped.glyphs[0].yOffset;
  const double nextPenY = runs[0].glyphs[2].yPosition - shaped.glyphs[2].yOffset;
  EXPECT_THAT(nextPenY, DoubleNear(basePenY + shaped.glyphs[0].yAdvance, 1e-6));
}

TEST(TextEngineTest, SidewaysCjkUsesHorizontalShapingMetrics) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "HarfBuzz shaping requires the full text backend.";
#endif

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  TextBackendFull backend(fontManager, registry);
  const std::string value = "\xE6\x97\xA5\xE6\x9C\xAC";

  const auto sideways = backend.shapeRun(font, 32.0f, value, 0, value.size(), true,
                                         TextOrientation::Sideways, FontVariant::Normal, false);
  ASSERT_THAT(sideways.glyphs, SizeIs(2));
  for (const auto& glyph : sideways.glyphs) {
    EXPECT_THAT(glyph.xAdvance, Gt(0.0));
    EXPECT_THAT(glyph.yAdvance, DoubleEq(0.0));
  }
}

TEST(TextEngineTest, VerticalCrossSpanKerningMatchesUnsplitOrientation) {
#ifndef DONNER_TEXT_FULL
  GTEST_SKIP() << "HarfBuzz shaping requires the full text backend.";
#endif

  for (const TextOrientation orientation : {TextOrientation::Upright, TextOrientation::Sideways}) {
    SCOPED_TRACE(testing::PrintToString(orientation));
    Registry registry;
    FontManager fontManager(registry);
    ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));
    TextEngine engine(fontManager, registry);

    auto layout = [&](bool split) {
      components::ComputedTextComponent text;
      auto first = MakeSpan(split ? "A" : "AV");
      first.textOrientation = orientation;
      text.spans.push_back(std::move(first));
      if (split) {
        auto second = MakeSpan("V");
        second.startsNewChunk = false;
        second.textOrientation = orientation;
        text.spans.push_back(std::move(second));
      }
      TextLayoutParams params = MakeTextParams(64.0);
      params.fontFamilies = {RcString("Noto Sans")};
      params.writingMode = WritingMode::VerticalRl;
      return engine.layout(text, params);
    };

    const auto unsplit = layout(false);
    const auto split = layout(true);
    ASSERT_THAT(unsplit, ElementsAre(RunGlyphsAre(SizeIs(2))));
    ASSERT_THAT(split, ElementsAre(RunGlyphsAre(SizeIs(1)), RunGlyphsAre(SizeIs(1))));
    EXPECT_THAT(split[1].glyphs[0].yPosition, DoubleNear(unsplit[0].glyphs[1].yPosition, 1e-6));
  }
}

TEST(TextEngineTest, TextPathSpacingPreservesFullShapingCombiningMarkCluster) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue()) << "Noto Sans fixture font must load";
  TextBackendFull backend(fontManager, registry);
  const std::string value =
      "q\xCC\x81"
      "B";
  const auto shaped =
      backend.shapeRun(font, 24.0f, value, 0, value.size(), false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(Field("cluster", &TextBackend::ShapedGlyph::cluster, 0u),
                                         Field("cluster", &TextBackend::ShapedGlyph::cluster, 0u),
                                         Field("cluster", &TextBackend::ShapedGlyph::cluster, 3u)));
  ASSERT_THAT(shaped.glyphs[1].xAdvance, DoubleEq(0.0));
  ASSERT_THAT(shaped.glyphs[1].glyphIndex, Not(0));
  const double clusterAdvance = shaped.glyphs[0].xAdvance + shaped.glyphs[1].xAdvance;
  const double target = clusterAdvance + shaped.glyphs[2].xAdvance + 40.0;
  components::ComputedTextComponent text;
  auto span = MakeSpan(value);
  span.pathSpline = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(500.0, 0.0)).build();
  span.textLength = Lengthd(target, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::Spacing;
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(24.0);
  params.fontFamilies = {RcString("Noto Sans")};
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));
  const auto runs = engine.layout(text, params);
  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(shaped.glyphs[0].xOffset, 1e-6)),
                GlyphYPositionIs(DoubleNear(shaped.glyphs[0].yOffset, 1e-6))),
          AllOf(GlyphXPositionIs(
                    DoubleNear(shaped.glyphs[0].xAdvance + shaped.glyphs[1].xOffset, 1e-6)),
                GlyphYPositionIs(DoubleNear(shaped.glyphs[1].yOffset, 1e-6))),
          GlyphXPositionIs(DoubleNear(clusterAdvance + shaped.glyphs[2].xOffset + 40.0, 1e-6))))));
}

TEST(TextEngineTest, TextPathStretchFollowsEachGlyphChord) {
  Registry registry;
  FontManager fontManager(registry);
  const Path path = PathBuilder()
                        .moveTo(Vector2d(0.0, 0.0))
                        .curveTo(Vector2d(0.0, 100.0), Vector2d(100.0, 100.0), Vector2d(100.0, 0.0))
                        .build();
  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.pathSpline = path;
  span.textPathMethod = components::TextPathMethod::Stretch;
  text.spans.push_back(std::move(span));

  TextEngine engine(fontManager, registry);
  const auto runs = engine.layout(text, MakeTextParams(24.0));

  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(2));
  ASSERT_TRUE(runs[0].stretchedTextPath);
  double expectedOrigin = 0.0;
  for (const TextGlyph& glyph : runs[0].glyphs) {
    ASSERT_TRUE(glyph.textPathOrigin);
    EXPECT_NEAR(*glyph.textPathOrigin, expectedOrigin, 1e-6);
    expectedOrigin += glyph.xAdvance;
  }
}

TEST(TextEngineTest, TextPathMeasurementRetentionUsesDocumentFamilyBudget) {
  Registry registry;
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.geometryBytes = 16 * 1024 * 1024;
  limits.maximumTotalRetainedBytes = 16 * 1024 * 1024;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  registry.ctx().emplace<components::DocumentResourceFamilyContext>(family);
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);
  const Path path = PathBuilder()
                        .moveTo(Vector2d(0.0, 0.0))
                        .curveTo(Vector2d(0.0, 1e7), Vector2d(1e7, 1e7), Vector2d(1e7, 0.0))
                        .build();
  std::vector<std::vector<TextRun>> retainedRuns;
  retainedRuns.reserve(32);
  std::size_t admitted = 0;
  for (int root = 0; root < 32; ++root) {
    components::ComputedTextComponent text;
    auto span = MakeSpan("A");
    span.pathSpline = path;
    span.textPathSourceEntity = registry.create();
    span.textPathMethod = components::TextPathMethod::Stretch;
    text.spans.push_back(std::move(span));
    auto runs = engine.layout(text, MakeTextParams(24.0));
    if (!runs.empty() && runs.front().stretchedTextPath) {
      ++admitted;
    }
    retainedRuns.push_back(std::move(runs));
  }

  EXPECT_THAT(admitted, AllOf(Gt(0u), Lt(32u)));
  EXPECT_LE(family->retainedBytes(components::DocumentResourceFamilyBudget::Kind::Geometry),
            limits.geometryBytes);
  EXPECT_FALSE(family->securityStats().rejected);
}

TEST(TextEngineTest, TextPathMeasurementWorkIsCumulativeAfterReleasedAlignPaths) {
  Registry registry;
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.textPathMeasurementWorkUnits = 4;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  registry.ctx().emplace<components::DocumentResourceFamilyContext>(family);
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(100.0, 0.0)).build();
  std::size_t admitted = 0;
  for (int root = 0; root < 5; ++root) {
    components::ComputedTextComponent text;
    auto span = MakeSpan("A");
    span.pathSpline = path;
    span.textPathSourceEntity = registry.create();
    text.spans.push_back(std::move(span));
    const auto runs = engine.layout(text, MakeTextParams(24.0));
    if (!runs.empty() && !runs.front().glyphs.empty()) {
      ++admitted;
    }
  }

  EXPECT_EQ(admitted, 2u);
  EXPECT_EQ(family->securityStats().textPathMeasurementWorkUnits, 4u);
  EXPECT_EQ(family->retainedBytes(components::DocumentResourceFamilyBudget::Kind::Geometry), 0u);
  EXPECT_TRUE(family->securityStats().textPathMeasurementWorkRejected);
}

TEST(TextEngineTest, TextPathStretchKeepsCombiningMarkInBaseClusterFrame) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));
  const std::string value = "q\xCC\x81";
  TextBackendFull backend(fontManager, registry);
  const auto shaped =
      backend.shapeRun(font, 24.0f, value, 0, value.size(), false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(2));
  components::ComputedTextComponent text;
  auto span = MakeSpan(value);
  span.pathSpline = PathBuilder()
                        .moveTo(Vector2d(0.0, 0.0))
                        .curveTo(Vector2d(0.0, 100.0), Vector2d(100.0, 100.0), Vector2d(100.0, 0.0))
                        .build();
  span.textPathMethod = components::TextPathMethod::Stretch;
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(24.0);
  params.fontFamilies = {RcString("Noto Sans")};
  TextEngine engine(fontManager, registry,
                    std::make_unique<TextBackendFull>(fontManager, registry));

  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(2));
  const TextGlyph& base = runs[0].glyphs[0];
  const TextGlyph& mark = runs[0].glyphs[1];
  ASSERT_TRUE(base.textPathOrigin);
  ASSERT_TRUE(mark.textPathOrigin);
  EXPECT_DOUBLE_EQ(mark.xAdvance, 0.0);
  EXPECT_NEAR(*mark.textPathOrigin - *base.textPathOrigin,
              shaped.glyphs[0].xAdvance + shaped.glyphs[1].xOffset - shaped.glyphs[0].xOffset,
              1e-6);
  EXPECT_NEAR(mark.textPathNormalOffset - base.textPathNormalOffset,
              shaped.glyphs[1].yOffset - shaped.glyphs[0].yOffset, 1e-6);
}

TEST(TextEngineTest, TextPathStretchMappingStopsAtItsOwningRunRange) {
  Registry registry;
  FontManager fontManager(registry);
  components::ComputedTextComponent text;
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();
  auto stretched = MakeSpan("A");
  stretched.pathSpline = path;
  stretched.textPathSourceEntity = registry.create();
  stretched.textPathMethod = components::TextPathMethod::Stretch;
  auto flat = MakeSpan("B");
  auto aligned = MakeSpan("C");
  aligned.pathSpline = path;
  aligned.textPathSourceEntity = registry.create();
  text.spans = {std::move(stretched), std::move(flat), std::move(aligned)};

  TextEngine engine(fontManager, registry);
  const auto runs = engine.layout(text, MakeTextParams(24.0));

  ASSERT_THAT(runs, SizeIs(3));
  EXPECT_TRUE(runs[0].stretchedTextPath);
  EXPECT_FALSE(runs[1].stretchedTextPath);
  EXPECT_FALSE(runs[2].stretchedTextPath);
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
                                                      GlyphYPositionIs(DoubleNear(-10.0, 1e-6)))));
}

// This exact Latin case preserves the e-x kerning result across a paint-only tspan boundary.
TEST(TextEngineTest, FullBackendPaintOnlySplitMatchesUnsplitGlyphPositions) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry);

  ASSERT_TRUE(static_cast<bool>(LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans")));

  components::ComputedTextComponent unsplitText;
  unsplitText.spans.push_back(MakeSpan("Text"));

  components::ComputedTextComponent splitText;
  splitText.spans.push_back(MakeSpan("Te"));
  auto trailingSpan = MakeSpan("xt");
  trailingSpan.startsNewChunk = false;
  trailingSpan.paintOrder.order = {PaintComponent::Stroke, PaintComponent::Fill,
                                   PaintComponent::Markers};
  splitText.spans.push_back(std::move(trailingSpan));

  TextLayoutParams params = MakeTextParams(64.0);
  params.fontFamilies = {RcString("Noto Sans")};
  const auto unsplitRuns = engine.layout(unsplitText, params);
  const auto splitRuns = engine.layout(splitText, params);

  ASSERT_THAT(unsplitRuns, ElementsAre(RunGlyphsAre(SizeIs(4))));
  ASSERT_THAT(splitRuns, ElementsAre(RunGlyphsAre(SizeIs(2)), RunGlyphsAre(SizeIs(2))));
  const auto splitGlyphAt = [&splitRuns](size_t index) -> const TextGlyph& {
    return index < 2 ? splitRuns[0].glyphs[index] : splitRuns[1].glyphs[index - 2];
  };
  for (size_t glyphIndex = 0; glyphIndex < 4; ++glyphIndex) {
    const TextGlyph& unsplitGlyph = unsplitRuns[0].glyphs[glyphIndex];
    const TextGlyph& splitGlyph = splitGlyphAt(glyphIndex);
    const Path unsplitOutline =
        engine.glyphOutline(unsplitRuns[0].font, unsplitGlyph.glyphIndex,
                            engine.scaleForEmToPixels(unsplitRuns[0].font, 64.0f));
    const Path splitOutline = engine.glyphOutline(
        splitRuns[glyphIndex < 2 ? 0 : 1].font, splitGlyph.glyphIndex,
        engine.scaleForEmToPixels(splitRuns[glyphIndex < 2 ? 0 : 1].font, 64.0f));
    ASSERT_THAT(unsplitOutline.commands(), Not(IsEmpty()));
    ASSERT_THAT(splitOutline.commands(), Not(IsEmpty()));
    const Box2d unsplitBounds = unsplitOutline.transformedBounds(
        Transform2d::Translate(Vector2d(unsplitGlyph.xPosition, unsplitGlyph.yPosition)));
    const Box2d splitBounds = splitOutline.transformedBounds(
        Transform2d::Translate(Vector2d(splitGlyph.xPosition, splitGlyph.yPosition)));

    EXPECT_EQ(splitGlyph.glyphIndex, unsplitGlyph.glyphIndex) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.xPosition, unsplitGlyph.xPosition) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.yPosition, unsplitGlyph.yPosition) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.yAdvance, unsplitGlyph.yAdvance) << glyphIndex;
    // Compare the placed advance so cross-span kerning has the same meaning regardless of
    // whether the backend folds it into xAdvance or applies it at the next glyph position.
    const double unsplitAdvance =
        glyphIndex + 1 < 4
            ? unsplitRuns[0].glyphs[glyphIndex + 1].xPosition - unsplitGlyph.xPosition
            : unsplitGlyph.xAdvance;
    const double splitAdvance = glyphIndex + 1 < 4
                                    ? splitGlyphAt(glyphIndex + 1).xPosition - splitGlyph.xPosition
                                    : splitGlyph.xAdvance;
    EXPECT_DOUBLE_EQ(splitAdvance, unsplitAdvance) << glyphIndex;
    EXPECT_THAT(splitBounds, BoxEq(Vector2Eq(DoubleNear(unsplitBounds.topLeft.x, 1e-9),
                                             DoubleNear(unsplitBounds.topLeft.y, 1e-9)),
                                   Vector2Eq(DoubleNear(unsplitBounds.bottomRight.x, 1e-9),
                                             DoubleNear(unsplitBounds.bottomRight.y, 1e-9))))
        << glyphIndex;
  }
}

TEST(TextEngineTest, TextAfterTextPathStartsAtGeometricEndpoint) {
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
              FirstGlyphMatches(GlyphXPositionIs(AllOf(Gt(lastPathGlyphX), DoubleEq(200.0)))));
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

TEST(TextEngineTest, WhiteSpaceControlsSoftWrap) {
  for (const auto mode : {WhiteSpace::Normal, WhiteSpace::Pre, WhiteSpace::Nowrap,
                          WhiteSpace::PreWrap, WhiteSpace::BreakSpaces, WhiteSpace::PreLine}) {
    SCOPED_TRACE(testing::PrintToString(mode));
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto span = MakeSpan("aaa bbb ccc");
    span.whiteSpace = mode;
    text.spans.push_back(std::move(span));
    auto params = MakeTextParams(20.0);
    params.inlineSizePx = 45.0;
    EXPECT_THAT(DistinctBaselines(engine.layout(text, params)),
                SizeIs(AllowsSoftWrap(mode) ? 3 : 1));
  }
}

TEST(TextEngineTest, WhiteSpacePreservesForcedBreaksWithoutInlineSize) {
  for (const auto mode :
       {WhiteSpace::Pre, WhiteSpace::PreWrap, WhiteSpace::BreakSpaces, WhiteSpace::PreLine}) {
    SCOPED_TRACE(testing::PrintToString(mode));
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto span = MakeSpan("A\n\nB");
    span.whiteSpace = mode;
    text.spans.push_back(std::move(span));
    const auto runs = engine.layout(text, MakeTextParams(20.0));
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs[0].glyphs, SizeIs(4));
    EXPECT_THAT(runs[0].glyphs[3].yPosition - runs[0].glyphs[0].yPosition, DoubleNear(40.0, 1e-5));
    EXPECT_THAT(runs[0].glyphs[3].xPosition, DoubleEq(runs[0].glyphs[0].xPosition));
    EXPECT_THAT(runs[0].glyphs[1].xAdvance, DoubleEq(0.0));
  }
}

TEST(TextEngineTest, WhiteSpaceBreakSpacesWrapsInsideSpaceRun) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  auto params = MakeTextParams(20.0);
  params.inlineSizePx = 25.0;
  components::ComputedTextComponent text;
  auto span = MakeSpan("A    B");
  span.whiteSpace = WhiteSpace::PreWrap;
  text.spans.push_back(span);
  const auto preWrap = engine.layout(text, params);
  text.spans[0].whiteSpace = WhiteSpace::BreakSpaces;
  const auto breakSpaces = engine.layout(text, params);
  EXPECT_THAT(DistinctBaselines(preWrap), SizeIs(2));
  EXPECT_THAT(DistinctBaselines(breakSpaces), SizeIs(3));
  EXPECT_THAT(breakSpaces[0].glyphs[2].yPosition, Gt(breakSpaces[0].glyphs[1].yPosition));
}

TEST(TextEngineTest, WhiteSpaceTabUsesEightSpaceStops) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan("A\tB");
  span.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(span);
  const auto tab = engine.layout(text, MakeTextParams(20.0));
  text.spans[0] = MakeSpan("A B");
  text.spans[0].whiteSpace = WhiteSpace::Pre;
  const auto space = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(tab, SizeIs(1));
  ASSERT_THAT(tab[0].glyphs, SizeIs(3));
  ASSERT_THAT(space, SizeIs(1));
  ASSERT_THAT(space[0].glyphs, SizeIs(3));
  EXPECT_THAT(tab[0].glyphs[2].xPosition, DoubleEq(space[0].glyphs[1].xAdvance * 8.0 + 1.0));
}

TEST(TextEngineTest, WhiteSpaceForcedBreakContinuesAcrossRuns) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  for (const auto* contents : {"A\n", "B", "\nC"}) {
    auto span = MakeSpan(contents);
    span.whiteSpace = WhiteSpace::Pre;
    span.startsNewChunk = false;
    text.spans.push_back(span);
  }
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(3));
  EXPECT_THAT(runs[1].glyphs,
              ElementsAre(testing::AllOf(GlyphXPositionIs(DoubleEq(0.0)),
                                         GlyphYPositionIs(DoubleNear(20.0, 1e-5)))));
  ASSERT_THAT(runs[2].glyphs, SizeIs(2));
  EXPECT_THAT(runs[2].glyphs[1].xPosition, DoubleEq(0.0));
  EXPECT_THAT(runs[2].glyphs[1].yPosition, DoubleNear(40.0, 1e-5));
}

TEST(TextEngineTest, WhiteSpaceVerticalForcedBreaksAndTabs) {
  for (const auto writingMode : {WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto span = MakeSpan("A\tB\nC");
    span.whiteSpace = WhiteSpace::Pre;
    span.textOrientation = TextOrientation::Upright;
    text.spans.push_back(span);
    auto params = MakeTextParams(20.0);
    params.writingMode = writingMode;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs[0].glyphs, SizeIs(5));
    EXPECT_THAT(runs[0].glyphs[1].yAdvance, Gt(12.0));
    EXPECT_THAT(runs[0].glyphs[4].yPosition, DoubleEq(runs[0].glyphs[0].yPosition));
    EXPECT_THAT(runs[0].glyphs[4].xPosition - runs[0].glyphs[0].xPosition,
                DoubleNear(writingMode == WritingMode::VerticalRl ? -20.0 : 20.0, 1e-5));
  }
}

TEST(TextEngineTest, WhiteSpaceFlatForcedBreakSurvivesTextPathSibling) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto pathSpan = MakeSpan("X");
  pathSpan.pathSpline = PathBuilder().moveTo({0, 0}).lineTo({200, 0}).build();
  text.spans.push_back(pathSpan);
  auto flatSpan = MakeSpan("A\nB");
  flatSpan.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(flatSpan);
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs[0].glyphs, SizeIs(1));
  ASSERT_THAT(runs[1].glyphs, SizeIs(3));
  EXPECT_THAT(runs[1].glyphs[2].yPosition - runs[1].glyphs[0].yPosition, DoubleNear(20.0, 1e-5));
  EXPECT_THAT(runs[1].glyphs[2].xPosition, DoubleEq(runs[1].glyphs[0].xPosition));
}

class TabStopTextBackend : public ScriptedTextBackend {
public:
  TabStopTextBackend(double leadingAdvance, double zeroAdvance)
      : leadingAdvance_(leadingAdvance), zeroAdvance_(zeroAdvance) {}

  ShapedRun shapeRun(FontHandle font, float size, std::string_view text, size_t offset,
                     size_t length, bool vertical, FontVariant variant,
                     bool logical) const override {
    auto run =
        ScriptedTextBackend::shapeRun(font, size, text, offset, length, vertical, variant, logical);
    for (auto& glyph : run.glyphs) {
      if (text[glyph.cluster] == 'A') {
        glyph.xAdvance = leadingAdvance_;
      } else if (text[glyph.cluster] == '0') {
        glyph.xAdvance = zeroAdvance_;
      }
    }
    return run;
  }

private:
  double leadingAdvance_;
  double zeroAdvance_;
};

TEST(TextEngineTest, WhiteSpaceTabSkipsStopCloserThanHalfCh) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry, std::make_unique<TabStopTextBackend>(76.5, 10));
  components::ComputedTextComponent text;
  auto span = MakeSpan("A\tB");
  span.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(3));
  EXPECT_THAT(runs[0].glyphs[1].xAdvance, Gt(80.0));
}

TEST(TextEngineTest, WhiteSpacePreWrapCountsSpacesBeforeForcedBreakForAnchor) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan("A \nB");
  span.whiteSpace = WhiteSpace::PreWrap;
  span.textAnchor = TextAnchor::End;
  text.spans.push_back(span);
  auto params = MakeTextParams(20.0);
  params.textAnchor = TextAnchor::End;
  params.inlineSizePx = 200.0;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(4));
  EXPECT_THAT(runs[0].glyphs[1].xPosition + runs[0].glyphs[1].xAdvance, DoubleEq(0.0));
  EXPECT_THAT(runs[0].glyphs[0].xPosition, testing::Lt(runs[0].glyphs[3].xPosition));
}

TEST(TextEngineTest, WhiteSpaceCommonAncestorControlsSpanBoundaryBreak) {
  for (const auto common : {WhiteSpace::Normal, WhiteSpace::Nowrap}) {
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto first = MakeSpan("AA ");
    first.whiteSpace = WhiteSpace::Normal;
    first.boundaryWhiteSpace = common;
    text.spans.push_back(first);
    text.spans.push_back(MakeSpan("BB"));
    auto params = MakeTextParams(20.0);
    params.inlineSizePx = 30.0;
    EXPECT_THAT(DistinctBaselines(engine.layout(text, params)),
                SizeIs(common == WhiteSpace::Normal ? 2 : 1));
  }
}

TEST(TextEngineTest, WhiteSpaceTextPathControlsShapeAsBlanks) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan("A\n\tB");
  span.whiteSpace = WhiteSpace::Pre;
  span.pathSpline = PathBuilder().moveTo({0, 0}).lineTo({200, 0}).build();
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(4));
  EXPECT_THAT(runs[0].glyphs[1].glyphIndex, testing::Eq(static_cast<int>(' ') + 1));
  EXPECT_THAT(runs[0].glyphs[2].glyphIndex, testing::Eq(static_cast<int>(' ') + 1));
}

TEST(TextEngineTest, WhiteSpaceTabRetainsTextLengthAndAnchor) {
  for (bool onSpan : {false, true}) {
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto span = MakeSpan("A\tB");
    span.whiteSpace = WhiteSpace::Pre;
    span.textAnchor = TextAnchor::End;
    auto params = MakeTextParams(20.0);
    params.textAnchor = TextAnchor::End;
    if (onSpan) {
      span.textLength = Lengthd(100.0);
      span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
    } else {
      params.textLength = Lengthd(100.0);
      params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
    }
    text.spans.push_back(span);
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs[0].glyphs, SizeIs(3));
    const auto& first = runs[0].glyphs.front();
    const auto& last = runs[0].glyphs.back();
    EXPECT_THAT(last.xPosition + last.xAdvance - first.xPosition, DoubleNear(100.0, 1e-5));
    EXPECT_THAT(last.xPosition + last.xAdvance, DoubleNear(0.0, 1e-5));
  }
}

TEST(TextEngineTest, WhiteSpaceTabMinimumUsesZeroAdvanceRatherThanSpace) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry, std::make_unique<TabStopTextBackend>(72, 20));
  components::ComputedTextComponent text;
  auto span = MakeSpan("A\tB");
  span.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(3));
  EXPECT_THAT(runs[0].glyphs[1].xAdvance, DoubleEq(87.0));
}

TEST(TextEngineTest, WrappedNestedSpanOwnsLineAnchors) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    for (auto anchor : {TextAnchor::Start, TextAnchor::Middle, TextAnchor::End}) {
      SCOPED_TRACE(testing::Message() << mode << " " << anchor);
      Registry registry;
      FontManager fonts(registry);
      TextEngine engine = MakeScriptedEngine(registry, fonts);
      components::ComputedTextComponent text;
      text.spans.push_back(MakeSpan(""));
      auto span = MakeSpan(isVertical(mode) ? "\xe7\x95\x8c\n\xe7\x95\x8c\xe7\x95\x8c" : "A\nBB");
      span.whiteSpace = WhiteSpace::Pre;
      span.textAnchor = anchor;
      span.textOrientation = TextOrientation::Upright;
      text.spans.push_back(span);
      auto params = MakeTextParams(20);
      params.writingMode = mode;
      params.textAnchor = anchor == TextAnchor::Start ? TextAnchor::End : TextAnchor::Start;
      const auto runs = engine.layout(text, params);
      ASSERT_THAT(runs, SizeIs(2));
      ASSERT_THAT(runs[1].glyphs, SizeIs(4));
      const auto pen = [mode](const TextGlyph& g) {
        return isVertical(mode) ? g.yPosition - g.verticalOriginOffset : g.xPosition;
      };
      const double advance = isVertical(mode) ? 12 : 10;
      const double kern = isVertical(mode) ? 2 : 1;
      const double factor = anchor == TextAnchor::Start    ? 0
                            : anchor == TextAnchor::Middle ? 0.5
                                                           : 1;
      EXPECT_THAT(pen(runs[1].glyphs[0]), DoubleNear(-factor * advance, 1e-5));
      EXPECT_THAT(pen(runs[1].glyphs[2]), DoubleNear(-factor * (2 * advance + kern), 1e-5));
    }
  }
}

TEST(TextEngineTest, WrappedLineUsesFirstSpanAnchorWithoutShearingMixedSpans) {
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  auto first = MakeSpan("A");
  first.whiteSpace = WhiteSpace::Pre;
  first.textAnchor = TextAnchor::End;
  auto second = MakeSpan("B\nCC");
  second.whiteSpace = WhiteSpace::Pre;
  second.startsNewChunk = false;
  second.textAnchor = TextAnchor::Middle;
  text.spans = {first, second};
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs[0].glyphs, SizeIs(1));
  ASSERT_THAT(runs[1].glyphs, SizeIs(4));
  EXPECT_THAT(runs[0].glyphs[0].xPosition, DoubleNear(-23, 1e-5));
  EXPECT_THAT(runs[1].glyphs[0].xPosition, DoubleNear(-10, 1e-5));
  EXPECT_THAT(runs[1].glyphs[2].xPosition, DoubleNear(-10.5, 1e-5));
}

TEST(TextEngineTest, WrappedExplicitPositionStartsIndependentAnchorChunk) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    SCOPED_TRACE(testing::PrintToString(mode));
    Registry registry;
    FontManager fonts(registry);
    TextEngine engine = MakeScriptedEngine(registry, fonts);
    components::ComputedTextComponent text;
    auto first = MakeSpan("A");
    first.whiteSpace = WhiteSpace::Pre;
    first.textAnchor = TextAnchor::Middle;
    first.textOrientation = TextOrientation::Upright;
    auto second = MakeSpan("BC\nD");
    second.whiteSpace = WhiteSpace::Pre;
    second.textAnchor = TextAnchor::End;
    second.textOrientation = TextOrientation::Upright;
    if (isVertical(mode)) {
      second.yList = {Lengthd(100)};
    } else {
      second.xList = {Lengthd(100)};
    }
    text.spans = {first, second};
    auto params = MakeTextParams(20);
    params.writingMode = mode;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(2));
    ASSERT_THAT(runs[0].glyphs, SizeIs(1));
    ASSERT_THAT(runs[1].glyphs, SizeIs(4));
    const auto pen = [mode](const TextGlyph& glyph) {
      return isVertical(mode) ? glyph.yPosition - glyph.verticalOriginOffset : glyph.xPosition;
    };
    const double advance = isVertical(mode) ? 12 : 10;
    const double kern = isVertical(mode) ? 2 : 1;
    EXPECT_THAT(pen(runs[0].glyphs[0]), DoubleNear(-advance / 2, 1e-5));
    EXPECT_THAT(pen(runs[1].glyphs[0]), DoubleNear(100 - 2 * advance - kern, 1e-5));
    EXPECT_THAT(pen(runs[1].glyphs[1]), DoubleNear(100 - advance, 1e-5));
    EXPECT_THAT(pen(runs[1].glyphs[3]) + advance, DoubleNear(100, 1e-5));
  }
}

TEST(TextEngineTest, WrappedEmptyLinesDoNotSelectTheFollowingAnchor) {
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  auto breaks = MakeSpan("\n\n");
  breaks.whiteSpace = WhiteSpace::Pre;
  breaks.textAnchor = TextAnchor::Start;
  auto visible = MakeSpan("BB");
  visible.whiteSpace = WhiteSpace::Pre;
  visible.startsNewChunk = false;
  visible.textAnchor = TextAnchor::End;
  text.spans = {breaks, visible};
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs[1].glyphs, SizeIs(2));
  EXPECT_THAT(runs[1].glyphs[0].xPosition, DoubleNear(-21, 1e-5));
}

TEST(TextEngineTest, WrappedSoftLineStartsUseTheNewFirstSpanAnchor) {
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  auto first = MakeSpan("A ");
  first.textAnchor = TextAnchor::End;
  auto second = MakeSpan("BB");
  second.startsNewChunk = false;
  second.textAnchor = TextAnchor::Middle;
  text.spans = {first, second};
  auto params = MakeTextParams(20);
  params.inlineSizePx = 30;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(2));
  EXPECT_THAT(runs[0].glyphs[0].xPosition, DoubleNear(-10, 1e-5));
  EXPECT_THAT(runs[1].glyphs[0].xPosition, DoubleNear(-10.5, 1e-5));
}

TEST(TextEngineTest, WrappedAnchorMeasuresBothEndsOfNegativeDisplacements) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    for (auto anchor : {TextAnchor::Start, TextAnchor::Middle, TextAnchor::End}) {
      SCOPED_TRACE(testing::Message() << mode << " " << anchor);
      Registry registry;
      FontManager fonts(registry);
      TextEngine engine = MakeScriptedEngine(registry, fonts);
      components::ComputedTextComponent text;
      auto span = MakeSpan("AB\nC");
      span.whiteSpace = WhiteSpace::Pre;
      span.textAnchor = anchor;
      span.textOrientation = TextOrientation::Upright;
      if (isVertical(mode)) {
        span.dyList = {std::nullopt, Lengthd(-40)};
      } else {
        span.dxList = {std::nullopt, Lengthd(-30)};
      }
      text.spans.push_back(span);
      auto params = MakeTextParams(20);
      params.writingMode = mode;
      const auto runs = engine.layout(text, params);
      ASSERT_THAT(runs, SizeIs(1));
      ASSERT_THAT(runs[0].glyphs, SizeIs(4));
      const auto pen = [mode](const TextGlyph& glyph) {
        return isVertical(mode) ? glyph.yPosition - glyph.verticalOriginOffset : glyph.xPosition;
      };
      const double minimum = isVertical(mode) ? -26 : -19;
      const double maximum = isVertical(mode) ? 12 : 10;
      const double expected = anchor == TextAnchor::Start ? -minimum
                              : anchor == TextAnchor::End ? -maximum
                                                          : -(minimum + maximum) / 2;
      EXPECT_THAT(pen(runs[0].glyphs[0]), DoubleNear(expected, 1e-5));
      EXPECT_THAT(pen(runs[0].glyphs[1]), DoubleNear(minimum + expected, 1e-5));
    }
  }
}

TEST(TextEngineTest, WrappedEmptyInitialSpanPreservesAddressedLineOrigin) {
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan(""));
  auto span = MakeSpan("AB\nC");
  span.whiteSpace = WhiteSpace::Pre;
  span.textAnchor = TextAnchor::End;
  span.xList = {Lengthd(100)};
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs[1].glyphs, SizeIs(4));
  EXPECT_THAT(runs[1].glyphs[3].xPosition + runs[1].glyphs[3].xAdvance, DoubleNear(100, 1e-5));
}

TEST(TextEngineTest, WrappedAndOrdinaryAnchorsSharePenExtentRules) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    for (auto anchor : {TextAnchor::Start, TextAnchor::Middle, TextAnchor::End}) {
      Registry registry;
      FontManager fonts(registry);
      TextEngine engine = MakeScriptedEngine(registry, fonts);
      components::ComputedTextComponent text;
      auto span = MakeSpan("AB");
      span.whiteSpace = WhiteSpace::Pre;
      span.textAnchor = anchor;
      span.textOrientation = TextOrientation::Upright;
      if (isVertical(mode)) {
        span.dyList = {std::nullopt, Lengthd(-40)};
      } else {
        span.dxList = {std::nullopt, Lengthd(-30)};
      }
      text.spans.push_back(span);
      auto params = MakeTextParams(20);
      params.writingMode = mode;
      const auto ordinary = engine.layout(text, params);
      text.spans[0].text = RcString("AB\nC");
      text.spans[0].end = 4;
      const auto wrapped = engine.layout(text, params);
      ASSERT_THAT(ordinary, SizeIs(1));
      ASSERT_THAT(wrapped, SizeIs(1));
      ASSERT_THAT(ordinary[0].glyphs, SizeIs(2));
      ASSERT_THAT(wrapped[0].glyphs, SizeIs(4));
      for (size_t i = 0; i < 2; ++i) {
        EXPECT_THAT(wrapped[0].glyphs[i].xPosition,
                    DoubleNear(ordinary[0].glyphs[i].xPosition, 1e-5));
        EXPECT_THAT(wrapped[0].glyphs[i].yPosition,
                    DoubleNear(ordinary[0].glyphs[i].yPosition, 1e-5));
      }
    }
  }
}

namespace {

double InlinePen(const TextGlyph& glyph, WritingMode mode) {
  return isVertical(mode) ? glyph.yPosition - glyph.verticalOriginOffset : glyph.xPosition;
}

double InlineAdvance(const TextGlyph& glyph, WritingMode mode) {
  return isVertical(mode) ? glyph.yAdvance : glyph.xAdvance;
}

double BlockPosition(const TextGlyph& glyph, WritingMode mode) {
  return isVertical(mode) ? glyph.xPosition : glyph.yPosition;
}

double AnchorFraction(TextAnchor anchor) {
  return anchor == TextAnchor::Start ? 0 : anchor == TextAnchor::Middle ? 0.5 : 1;
}

components::ComputedTextComponent MakeAxisResetText(WritingMode mode, bool inlineReset,
                                                    bool blockReset, double blockValue) {
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan(""));
  auto first = MakeSpan("A\n");
  first.whiteSpace = WhiteSpace::Pre;
  first.textOrientation = TextOrientation::Upright;
  auto second = MakeSpan("B\nC");
  second.whiteSpace = WhiteSpace::Pre;
  second.textOrientation = TextOrientation::Upright;
  auto& inlinePositions = isVertical(mode) ? second.yList : second.xList;
  auto& blockPositions = isVertical(mode) ? second.xList : second.yList;
  if (inlineReset) {
    inlinePositions = {Lengthd(100)};
  }
  if (blockReset) {
    blockPositions = {Lengthd(blockValue)};
  }
  text.spans.push_back(first);
  text.spans.push_back(second);
  return text;
}

components::ComputedTextComponent MakeRelativeShiftText(WritingMode mode, TextAnchor anchor,
                                                        double delta, int sourceLayout) {
  components::ComputedTextComponent text;
  auto span = MakeSpan(sourceLayout == 0 ? "A\nB\nC" : "B\nC");
  span.whiteSpace = WhiteSpace::Pre;
  span.textOrientation = TextOrientation::Upright;
  span.textAnchor = anchor;
  const size_t b = sourceLayout == 0 ? 2 : 0;
  auto& relative = isVertical(mode) ? span.dyList : span.dxList;
  relative.resize(b + 1);
  relative[b] = Lengthd(delta);
  if (sourceLayout != 0) {
    auto prefix = MakeSpan("A\n");
    prefix.whiteSpace = WhiteSpace::Pre;
    prefix.textOrientation = TextOrientation::Upright;
    text.spans.push_back(prefix);
    span.startsNewChunk = sourceLayout == 2;
    if (sourceLayout == 2) {
      (isVertical(mode) ? span.xList : span.yList) = {Lengthd(100)};
    }
  }
  text.spans.push_back(span);
  return text;
}

void SetFixtureTextLength(components::ComputedTextComponent::TextSpan& span,
                          TextLayoutParams& params, int owner) {
  if (owner == 1) {
    params.textLength = Lengthd(200);
    params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  } else if (owner == 2) {
    span.textLength = Lengthd(200);
    span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  }
}

class WrappedAbsoluteAxisTest : public testing::TestWithParam<std::tuple<WritingMode, int>> {};
class WrappedRelativeShiftTest
    : public testing::TestWithParam<std::tuple<WritingMode, TextAnchor, double, int>> {};
class PreWrapAnchorTest
    : public testing::TestWithParam<std::tuple<WritingMode, TextAnchor, int, double>> {};

}  // namespace

TEST_P(WrappedAbsoluteAxisTest, WrappedAbsoluteAxesRemainIndependentAfterNewline) {
  const auto [mode, axes] = GetParam();
  const bool inlineReset = axes == 1 || axes == 3;
  const bool blockReset = axes != 1;
  const double blockValue = axes == 4 ? 0 : 100;
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  const auto text = MakeAxisResetText(mode, inlineReset, blockReset, blockValue);
  auto params = MakeTextParams(20);
  params.writingMode = mode;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(3));
  ASSERT_THAT(runs[2].glyphs, SizeIs(3));
  const double delta = mode == WritingMode::VerticalRl ? -20 : 20;
  const double expectedBlock = blockReset ? blockValue : delta;
  for (size_t i : {size_t(0), size_t(2)}) {
    const auto& glyph = runs[2].glyphs[i];
    EXPECT_THAT(InlinePen(glyph, mode), DoubleNear(inlineReset ? 100 : 0, 1e-5));
    EXPECT_THAT(BlockPosition(glyph, mode), DoubleNear(expectedBlock + (i == 2 ? delta : 0), 1e-5));
  }
}

INSTANTIATE_TEST_SUITE_P(AllWritingModes, WrappedAbsoluteAxisTest,
                         testing::Combine(testing::Values(WritingMode::HorizontalTb,
                                                          WritingMode::VerticalRl,
                                                          WritingMode::VerticalLr),
                                          testing::Values(1, 2, 3, 4)));

TEST(TextEngineTest, WrappedAnchorsPreserveVisualClusterOrder) {
  for (auto anchor : {TextAnchor::Start, TextAnchor::Middle, TextAnchor::End}) {
    SCOPED_TRACE(testing::PrintToString(anchor));
    Registry registry;
    FontManager fonts(registry);
    TextEngine engine = MakeScriptedEngine(registry, fonts, true);
    components::ComputedTextComponent text;
    auto span = MakeSpan("AB");
    span.whiteSpace = WhiteSpace::Pre;
    span.textAnchor = anchor;
    text.spans.push_back(span);
    const auto ordinary = engine.layout(text, MakeTextParams(20));
    auto lineBreak = MakeSpan("\n");
    lineBreak.whiteSpace = WhiteSpace::Pre;
    lineBreak.startsNewChunk = false;
    text.spans.push_back(lineBreak);
    span.startsNewChunk = false;
    text.spans.push_back(span);
    const auto wrapped = engine.layout(text, MakeTextParams(20));
    ASSERT_THAT(ordinary, SizeIs(1));
    ASSERT_THAT(ordinary[0].glyphs, SizeIs(2));
    ASSERT_THAT(wrapped, SizeIs(3));
    for (size_t run : {size_t(0), size_t(2)}) {
      ASSERT_THAT(wrapped[run].glyphs, SizeIs(2));
      EXPECT_THAT(wrapped[run].glyphs[0].cluster, testing::Eq(1u));
      EXPECT_THAT(wrapped[run].glyphs[1].cluster, testing::Eq(0u));
      for (size_t i = 0; i < 2; ++i) {
        EXPECT_THAT(wrapped[run].glyphs[i].xPosition,
                    DoubleNear(ordinary[0].glyphs[i].xPosition, 1e-5));
      }
    }
  }
}

TEST(TextEngineTest, WrappedBlockResetIsReappliedAfterSoftWrap) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    SCOPED_TRACE(testing::PrintToString(mode));
    Registry registry;
    FontManager fonts(registry);
    TextEngine engine = MakeScriptedEngine(registry, fonts);
    components::ComputedTextComponent text;
    auto first = MakeSpan("A ");
    first.textOrientation = TextOrientation::Upright;
    auto second = MakeSpan("BB");
    second.textOrientation = TextOrientation::Upright;
    if (isVertical(mode)) {
      second.xList = {Lengthd(100)};
    } else {
      second.yList = {Lengthd(100)};
    }
    text.spans = {first, second};
    auto params = MakeTextParams(20);
    params.writingMode = mode;
    params.inlineSizePx = 30;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(2));
    ASSERT_THAT(runs[1].glyphs, SizeIs(2));
    const auto& glyph = runs[1].glyphs[0];
    EXPECT_THAT(isVertical(mode) ? glyph.xPosition : glyph.yPosition, DoubleNear(100, 1e-5));
    EXPECT_THAT(isVertical(mode) ? glyph.yPosition - glyph.verticalOriginOffset : glyph.xPosition,
                DoubleNear(0, 1e-5));
  }
}

TEST(TextEngineTest, WrappedAnchorQuarantinesOnlyUnrepresentableGroups) {
  for (bool multiline : {false, true}) {
    for (bool separateRuns : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "multiline=" << multiline << " separateRuns=" << separateRuns);
      Registry registry;
      FontManager fonts(registry);
      TextEngine engine = MakeScriptedEngine(registry, fonts);
      components::ComputedTextComponent text;
      auto invalid = MakeSpan(multiline ? "AB\n" : "AB");
      invalid.whiteSpace = WhiteSpace::Pre;
      invalid.xList = {Lengthd(1e308)};
      invalid.dxList = {std::nullopt, Lengthd(-1e308)};
      if (separateRuns) {
        auto valid = MakeSpan("C");
        valid.whiteSpace = WhiteSpace::Pre;
        valid.xList = {Lengthd(25)};
        text.spans = {invalid, valid};
      } else {
        invalid.text = RcString(multiline ? "AB\nC" : "ABC");
        invalid.end = invalid.text.size();
        invalid.xList.resize(invalid.text.size());
        invalid.xList.back() = Lengthd(25);
        text.spans = {invalid};
      }
      const auto runs = engine.layout(text, MakeTextParams(20));
      ASSERT_THAT(runs, SizeIs(separateRuns ? 2 : 1));
      if (separateRuns) {
        EXPECT_THAT(runs.front().glyphs, IsEmpty());
      }
      ASSERT_THAT(runs.back().glyphs, SizeIs(1));
      EXPECT_THAT(runs.back().glyphs[0].xPosition, DoubleEq(25));
      EXPECT_THAT(runs.back().glyphs[0].yPosition, DoubleNear(multiline ? 20 : 0, 1e-5));
      EXPECT_THAT(runs.back().glyphs[0].cluster, testing::Eq(separateRuns ? 0u
                                                             : multiline  ? 3u
                                                                          : 2u));
    }
  }
}

TEST(TextEngineTest, RejectedAnchorChunksPreserveAlternatingValidGlyphs) {
  constexpr size_t kChunks = 128;
  std::string source;
  for (size_t i = 0; i < kChunks; ++i) {
    source += "ABC";
  }
  auto span = MakeSpan(source);
  span.xList.resize(source.size());
  span.dxList.resize(source.size());
  std::vector<std::pair<uint32_t, double>> expected;
  for (size_t i = 0; i < kChunks; ++i) {
    span.xList[3 * i] = Lengthd(1e308);
    span.dxList[3 * i + 1] = Lengthd(-1e308);
    span.xList[3 * i + 2] = Lengthd(25.0 + static_cast<double>(i));
    expected.emplace_back(static_cast<uint32_t>(3 * i + 2), 25.0 + static_cast<double>(i));
  }
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(1));
  std::vector<std::pair<uint32_t, double>> actual;
  for (const auto& glyph : runs[0].glyphs) {
    actual.emplace_back(glyph.cluster, glyph.xPosition);
  }
  EXPECT_THAT(actual, testing::ElementsAreArray(expected));
}

TEST_P(WrappedRelativeShiftTest, WrappedRelativeInlineShiftSurvivesFollowingLines) {
  const auto [mode, anchor, delta, sourceLayout] = GetParam();
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  const auto text = MakeRelativeShiftText(mode, anchor, delta, sourceLayout);
  auto params = MakeTextParams(20);
  params.writingMode = mode;
  const auto runs = engine.layout(text, params);
  const auto& glyphs = runs.back().glyphs;
  ASSERT_THAT(glyphs, SizeIs(sourceLayout == 0 ? 5 : 3));
  const double advance = isVertical(mode) ? 12 : 10;
  const size_t b = sourceLayout == 0 ? 2 : 0;
  for (size_t i : {b, b + 2}) {
    EXPECT_THAT(InlinePen(glyphs[i], mode),
                DoubleNear(delta - AnchorFraction(anchor) * advance, 1e-5));
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllWritingModes, WrappedRelativeShiftTest,
    testing::Combine(testing::Values(WritingMode::HorizontalTb, WritingMode::VerticalRl,
                                     WritingMode::VerticalLr),
                     testing::Values(TextAnchor::Start, TextAnchor::Middle, TextAnchor::End),
                     testing::Values(20.0, -20.0), testing::Values(0, 1, 2)));

TEST_P(PreWrapAnchorTest, SinglePreWrapLineHangsOverflowingSpacesAfterTextLength) {
  const auto [mode, anchor, lengthOwner, measure] = GetParam();
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  components::ComputedTextComponent text;
  auto span = MakeSpan("A  ");
  span.whiteSpace = WhiteSpace::PreWrap;
  span.textAnchor = anchor;
  span.textOrientation = TextOrientation::Upright;
  auto params = MakeTextParams(20);
  params.writingMode = mode;
  params.inlineSizePx = measure;
  SetFixtureTextLength(span, params, lengthOwner);
  text.spans.push_back(span);
  const auto actual = engine.layout(text, params);
  params.inlineSizePx = 0;
  text.spans[0].whiteSpace = WhiteSpace::Pre;
  const auto ordinary = engine.layout(text, params);
  ASSERT_THAT(actual, SizeIs(1));
  ASSERT_THAT(ordinary, SizeIs(1));
  ASSERT_THAT(actual[0].glyphs, SizeIs(3));
  const auto& first = actual[0].glyphs[0];
  const auto& reference = ordinary[0].glyphs[0];
  const double expected = measure == 1 ? -AnchorFraction(anchor) * InlineAdvance(reference, mode)
                                       : InlinePen(reference, mode);
  EXPECT_THAT(InlinePen(first, mode), DoubleNear(expected, 1e-5));
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_THAT(InlineAdvance(actual[0].glyphs[i], mode),
                DoubleNear(InlineAdvance(ordinary[0].glyphs[i], mode), 1e-5));
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllWritingModes, PreWrapAnchorTest,
    testing::Combine(testing::Values(WritingMode::HorizontalTb, WritingMode::VerticalRl,
                                     WritingMode::VerticalLr),
                     testing::Values(TextAnchor::Start, TextAnchor::Middle, TextAnchor::End),
                     testing::Values(0, 1, 2), testing::Values(1.0, 100.0)));

TEST(TextEngineTest, WhiteSpaceTabStopsPrecedeSvgInlineDisplacement) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    SCOPED_TRACE(testing::PrintToString(mode));
    Registry registry;
    FontManager fonts(registry);
    TextEngine engine = MakeScriptedEngine(registry, fonts);
    components::ComputedTextComponent text;
    auto span = MakeSpan("A\tB");
    span.whiteSpace = WhiteSpace::Pre;
    span.textOrientation = TextOrientation::Upright;
    auto& relative = isVertical(mode) ? span.dyList : span.dxList;
    relative.resize(3);
    relative[1] = Lengthd(62);
    text.spans.push_back(span);
    auto params = MakeTextParams(20);
    params.writingMode = mode;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs[0].glyphs, SizeIs(3));
    const auto& tab = runs[0].glyphs[1];
    const auto& b = runs[0].glyphs[2];
    EXPECT_THAT(isVertical(mode) ? tab.yAdvance : tab.xAdvance,
                DoubleNear(isVertical(mode) ? 82 : 69, 1e-5));
    EXPECT_THAT(isVertical(mode) ? b.yPosition - b.verticalOriginOffset : b.xPosition,
                DoubleNear(isVertical(mode) ? 160 : 143, 1e-5));
  }
}

namespace {

#ifdef DONNER_TEXT_FULL
TEST(TextEngineTest, RtlPreservedNewlineKeepsHebrewLinesInSourceOrder) {
  Registry registry;
  FontManager fonts(registry);
  const FontHandle font = LoadResvgFont(fonts, "MPLUS1p-Regular.ttf", "Mplus Hebrew");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  TextEngine engine(fonts, registry, std::make_unique<TextBackendFull>(fonts, registry));
  components::ComputedTextComponent text;
  auto span = MakeSpan("\xD7\x90\xD7\x91\n\xD7\x92\xD7\x93");
  span.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(span);
  auto params = MakeTextParams(20);
  params.fontFamilies = {RcString("Mplus Hebrew")};
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(5))));
  EXPECT_THAT(runs[0].font, testing::Eq(font));
  EXPECT_THAT(runs[0].glyphs,
              Each(AllOf(GlyphIndexIs(Gt(0)), Field(&TextGlyph::font, FontHandle()))));
  std::vector<uint32_t> clusters;
  for (const auto& glyph : runs[0].glyphs) {
    clusters.push_back(glyph.cluster);
  }
  EXPECT_THAT(clusters, ElementsAre(2u, 0u, 4u, 7u, 5u));
  EXPECT_THAT(runs[0].glyphs[0].yPosition, DoubleNear(0, 1e-5));
  // Pinned Mplus hhea: (1075 + 320 + 90) / 1000 em.
  EXPECT_THAT(runs[0].glyphs[3].yPosition, DoubleNear(29.7, 1e-5));
}

TEST(TextEngineTest, RtlPreservedNewlineKeepsArabicFallbackLinesInSourceOrder) {
  Registry registry;
  FontManager fonts(registry);
  const FontHandle primary = LoadResvgFont(fonts, "NotoSans-Regular.ttf", "Latin primary");
  const FontHandle fallback = LoadResvgFont(fonts, "Amiri-Regular.ttf", "Arabic fallback");
  ASSERT_THAT(static_cast<bool>(primary), testing::IsTrue());
  ASSERT_THAT(static_cast<bool>(fallback), testing::IsTrue());
  TextEngine engine(fonts, registry, std::make_unique<TextBackendFull>(fonts, registry));
  components::ComputedTextComponent text;
  auto span = MakeSpan("\xD8\xA8\xD8\xA8\n\xD8\xAA\xD8\xAA");
  span.whiteSpace = WhiteSpace::Pre;
  span.fontFamilies = {RcString("Latin primary"), RcString("Arabic fallback")};
  text.spans.push_back(span);
  auto params = MakeTextParams(20);
  params.fontFamilies = span.fontFamilies;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(5))));
  EXPECT_THAT(runs[0].font, testing::Eq(primary));
  EXPECT_THAT(runs[0].glyphs[0].font, testing::Eq(fallback));
  EXPECT_THAT(runs[0].glyphs[3].font, testing::Eq(fallback));
  std::vector<uint32_t> clusters;
  for (const auto& glyph : runs[0].glyphs) {
    EXPECT_THAT(glyph.glyphIndex, Gt(0));
    clusters.push_back(glyph.cluster);
  }
  EXPECT_THAT(clusters, ElementsAre(2u, 0u, 4u, 7u, 5u));
  // Root Noto hhea owns the line height: (1069 + 293) / 1000 em.
  EXPECT_THAT(runs[0].glyphs[3].yPosition - runs[0].glyphs[0].yPosition, DoubleNear(27.24, 1e-5));
}

#endif

}  // namespace

}  // namespace donner::svg
