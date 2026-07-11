#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Utf8.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/FontFace.h"
#include "donner/css/Specificity.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {
namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::FloatEq;
using ::testing::Gt;
using ::testing::IsEmpty;
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

class ScriptedTextBackend : public TextBackend {
public:
  bool reverseClusters = false;
  std::optional<SubSuperMetrics> subSuper;

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
    return subSuper;
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
      if (codepoint == 0x0301) {
        glyph.xAdvance = 0.0;
      }
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
                              bool reverseClusters = false,
                              std::optional<SubSuperMetrics> subSuper = std::nullopt) {
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->reverseClusters = reverseClusters;
  backend->subSuper = subSuper;
  return TextEngine(fontManager, registry, std::move(backend));
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

auto RunGlyphsAre(auto matcher) {
  return Field("glyphs", &TextRun::glyphs, matcher);
}

auto RunOnPathIs(auto matcher) {
  return Field("onPath", &TextRun::onPath, matcher);
}

}  // namespace

TEST(TextEngineScriptedTest, AddFontFacesOnlyRegistersNewBatchEntries) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  std::vector<css::FontFace> faces;
  css::FontFace first;
  first.familyName = RcString("First");
  faces.push_back(first);
  css::FontFace second;
  second.familyName = RcString("Second");
  faces.push_back(second);

  engine.addFontFaces(faces);
  EXPECT_EQ(fontManager.numFaces(), 2u);

  engine.addFontFaces(faces);
  EXPECT_EQ(fontManager.numFaces(), 2u);

  css::FontFace third;
  third.familyName = RcString("Third");
  faces.push_back(third);
  engine.addFontFaces(faces);
  EXPECT_EQ(fontManager.numFaces(), 3u);
}

TEST(TextEngineScriptedTest, CachedGeometryReturnsEmptyCacheWhenRequiredComponentsAreMissing) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);

  const EntityHandle rootHandle(registry, root);
  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 0);
  EXPECT_THAT(engine.computedGlyphPaths(rootHandle), IsEmpty());
  EXPECT_EQ(engine.computedObjectBoundingBox(rootHandle), Box2d());
}

TEST(TextEngineScriptedTest, CachedGeometryBuildsCharacterGlyphAndBoundsData) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);
  registry.emplace<components::ComputedViewBoxComponent>(root,
                                                         Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));

  components::TextComponent textComponent;
  textComponent.text = RcString(
      "A\xCC\x81"
      "B");
  textComponent.textLength = Lengthd(72.0, Lengthd::Unit::None);
  textComponent.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  registry.emplace<components::TextComponent>(root, textComponent);

  components::ComputedTextComponent computedText;
  auto span = MakeSpan(
      "A\xCC\x81"
      "B");
  span.sourceEntity = root;
  span.rotateList = {30.0, 30.0};
  computedText.spans.push_back(std::move(span));
  registry.emplace<components::ComputedTextComponent>(root, computedText);

  components::ComputedStyleComponent style;
  style.properties.emplace();
  registry.emplace<components::ComputedStyleComponent>(root, style);

  const EntityHandle rootHandle(registry, root);
  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 2);

  const std::vector<Path> glyphPaths = engine.computedGlyphPaths(rootHandle);
  EXPECT_THAT(glyphPaths, SizeIs(3));
  EXPECT_THAT(engine.computedInkBounds(rootHandle).width(), Gt(0.0));
  EXPECT_THAT(engine.computedObjectBoundingBox(rootHandle).height(), Gt(0.0));
  EXPECT_THAT(engine.getComputedTextLength(rootHandle), Gt(0.0));
  EXPECT_THAT(engine.getSubStringLength(rootHandle, 0, 1), Gt(0.0));
  EXPECT_EQ(engine.getStartPositionOfChar(rootHandle, 2), Vector2d());
  EXPECT_NE(engine.getRotationOfChar(rootHandle, 0), 0.0);
  EXPECT_THAT(engine.getExtentOfChar(rootHandle, 0).width(), Gt(0.0));
  EXPECT_EQ(engine.getCharNumAtPosition(rootHandle, engine.getStartPositionOfChar(rootHandle, 0)),
            0);
}

TEST(TextEngineScriptedTest, HorizontalLayoutCoversChunkAndSpanPositioningBranches) {
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

TEST(TextEngineScriptedTest, SupplementaryCoordinateListsUseLowSurrogateSlot) {
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

TEST(TextEngineScriptedTest, SupplementaryLowSurrogateCoordinatesApplyToFollowingGlyph) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  std::string textValue;
  Utf8::Append(U'\U0001F600', std::back_inserter(textValue));
  textValue += "B";

  components::ComputedTextComponent text;
  auto span = MakeSpan(textValue);
  span.xList = {Lengthd(0.0, Lengthd::Unit::None), Lengthd(70.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(0.0, Lengthd::Unit::None), Lengthd(90.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(
                  _, AllOf(GlyphXPositionIs(DoubleEq(70.0)), GlyphYPositionIs(DoubleEq(90.0)))))));
}

TEST(TextEngineScriptedTest, VerticalLayoutCoversSidewaysUprightSpacingAndRotation) {
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

TEST(TextEngineScriptedTest, VerticalLayoutStartsNewChunksForLaterAbsoluteCoordinates) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("ABCD");
  span.xList = {std::nullopt, Lengthd(50.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(80.0, Lengthd::Unit::None)};
  span.yList = {std::nullopt, Lengthd(20.0, Lengthd::Unit::None), std::nullopt,
                Lengthd(70.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(4))));
  EXPECT_THAT(runs[0].glyphs[1].xPosition, Gt(runs[0].glyphs[0].xPosition));
  EXPECT_THAT(runs[0].glyphs[1], GlyphYPositionIs(DoubleEq(20.0)));
  EXPECT_THAT(runs[0].glyphs[3].xPosition, Gt(runs[0].glyphs[1].xPosition));
  EXPECT_THAT(runs[0].glyphs[3], GlyphYPositionIs(DoubleEq(70.0)));
}

TEST(TextEngineScriptedTest, RtlChunkYOverrideKeepsVisualGlyphsOnSameBaseline) {
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

TEST(TextEngineScriptedTest, TextAnchorAdjustsIndependentHorizontalChunks) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto middle = MakeSpan("AB");
  middle.textAnchor = TextAnchor::Middle;

  auto end = MakeSpan("CD");
  end.xList = {Lengthd(50.0, Lengthd::Unit::None)};
  end.textAnchor = TextAnchor::End;

  text.spans.push_back(std::move(middle));
  text.spans.push_back(std::move(end));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(-10.5, 0.001)),
                                                   GlyphXPositionIs(DoubleNear(0.5, 0.001)))),
                          RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(29.0, 0.001)),
                                                   GlyphXPositionIs(DoubleNear(40.0, 0.001))))));
}

TEST(TextEngineScriptedTest, HiddenAndFailedSpansResetLayoutContinuity) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto hidden = MakeSpan("A");
  hidden.hidden = true;

  auto failedPath = MakeSpan("B");
  failedPath.textPathFailed = true;

  auto visible = MakeSpan("C");
  visible.startsNewChunk = false;

  text.spans.push_back(std::move(hidden));
  text.spans.push_back(std::move(failedPath));
  text.spans.push_back(std::move(visible));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty()), RunGlyphsAre(IsEmpty()),
                                RunGlyphsAre(ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(0.0)),
                                                               GlyphYPositionIs(DoubleEq(0.0)))))));
}

TEST(TextEngineScriptedTest, TextPathUsesAnchorContinuationAndVisibility) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const entt::entity textPathEntity = registry.create();
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();

  components::ComputedTextComponent text;
  auto first = MakeSpan("AB");
  first.pathSpline = path;
  first.textPathSourceEntity = textPathEntity;
  first.pathStartOffset = 10.0;
  first.textAnchor = TextAnchor::Middle;

  auto continued = MakeSpan("C");
  continued.startsNewChunk = false;
  continued.pathSpline = path;
  continued.textPathSourceEntity = textPathEntity;

  auto hidden = MakeSpan("D");
  hidden.pathSpline = path;
  hidden.textPathSourceEntity = textPathEntity;
  hidden.pathStartOffset = 80.0;
  hidden.visibility = Visibility::Hidden;

  text.spans.push_back(std::move(first));
  text.spans.push_back(std::move(continued));
  text.spans.push_back(std::move(hidden));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(
      runs, ElementsAre(AllOf(RunOnPathIs(Eq(true)),
                              RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(-0.5, 0.001)),
                                                       GlyphXPositionIs(DoubleNear(10.5, 0.001))))),
                        AllOf(RunOnPathIs(Eq(true)),
                              RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(20.5, 0.001))))),
                        AllOf(RunOnPathIs(Eq(true)), RunGlyphsAre(IsEmpty()))));
}

TEST(TextEngineScriptedTest, PerSpanTextLengthSpacingAndGlyphsScalesHorizontalAdvances) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("ABC");
  span.textLength = Lengthd(64.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphXAdvanceIs(DoubleEq(20.0)),
                              GlyphStretchScaleXIs(FloatEq(2.0f))),
                        AllOf(GlyphXPositionIs(DoubleEq(22.0)), GlyphXAdvanceIs(DoubleEq(20.0))),
                        GlyphXPositionIs(DoubleEq(44.0))))));
}

TEST(TextEngineScriptedTest, GlobalTextLengthSpacingAdjustsVerticalGlyphGaps) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("AB"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  params.textLength = Lengthd(44.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::Spacing;

  const auto runs = engine.layout(text, params);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphYPositionIs(DoubleEq(0.0)), GlyphYAdvanceIs(DoubleEq(10.0)),
                              GlyphStretchScaleYIs(FloatEq(1.0f))),
                        GlyphYPositionIs(DoubleEq(34.0))))));
}

TEST(TextEngineScriptedTest, RotatedCombiningMarkOffsetsRotateAroundBaseGlyph) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("A\xCC\x81");
  span.rotateList = {90.0, 90.0};
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(2))));
  const TextGlyph& base = runs[0].glyphs[0];
  const TextGlyph& mark = runs[0].glyphs[1];
  EXPECT_THAT(mark.xPosition, DoubleNear(base.xPosition, 1e-6));
  EXPECT_THAT(mark.yPosition, DoubleNear(base.yPosition + 11.0, 1e-6));
}

TEST(TextEngineScriptedTest, GlobalTextLengthSpacingAndGlyphsScalesVerticalAdvances) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("AB"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  params.textLength = Lengthd(44.0, Lengthd::Unit::None);
  params.lengthAdjust = LengthAdjust::SpacingAndGlyphs;

  const auto runs = engine.layout(text, params);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphYPositionIs(DoubleEq(0.0)), GlyphYAdvanceIs(DoubleEq(20.0)),
                              GlyphStretchScaleYIs(FloatEq(2.0f))),
                        GlyphYPositionIs(DoubleEq(24.0))))));
}

TEST(TextEngineScriptedTest, SubSuperMetricsResolveSpanAndAncestorBaselineShifts) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine =
      MakeScriptedEngine(registry, fontManager, /*reverseClusters=*/false,
                         SubSuperMetrics{.subscriptYOffset = 300, .superscriptYOffset = 500});

  using BaselineShiftKeyword = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  components::ComputedTextComponent text;

  auto sub = MakeSpan("A");
  sub.baselineShiftKeyword = BaselineShiftKeyword::Sub;

  auto super = MakeSpan("B");
  super.baselineShiftKeyword = BaselineShiftKeyword::Super;

  auto inherited = MakeSpan("C");
  inherited.ancestorBaselineShifts.push_back(
      {BaselineShiftKeyword::Sub, Lengthd(-0.33, Lengthd::Unit::Em), 20.0});
  inherited.ancestorBaselineShifts.push_back(
      {BaselineShiftKeyword::Super, Lengthd(0.4, Lengthd::Unit::Em), 20.0});

  text.spans.push_back(std::move(sub));
  text.spans.push_back(std::move(super));
  text.spans.push_back(std::move(inherited));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(6.0, 1e-6)))),
                          RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(-10.0, 1e-6)))),
                          RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleNear(-4.0, 1e-6))))));
}

TEST(TextEngineScriptedTest, TextPathEndAnchorAndExhaustedPathHideGlyphs) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const entt::entity textPathEntity = registry.create();
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(5.0, 0.0)).build();

  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.pathSpline = path;
  span.textPathSourceEntity = textPathEntity;
  span.pathStartOffset = 100.0;
  span.textAnchor = TextAnchor::End;
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(AllOf(
                        RunOnPathIs(Eq(true)),
                        RunGlyphsAre(ElementsAre(GlyphIndexIs(Eq(0)), GlyphIndexIs(Eq(0)))))));
}

// -- Error paths and edge branches (coverage-focused, all default-config) -----

namespace {

/// Backend that shapes non-ASCII codepoints to .notdef unless shaped with `coveringFont`,
/// for exercising the registered-face coverage-fallback search.
class CoverageProbeBackend : public ScriptedTextBackend {
public:
  FontHandle coveringFont;

  ShapedRun shapeRun(FontHandle font, float fontSizePx, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool isVertical,
                     FontVariant fontVariant, bool forceLogicalOrder) const override {
    ShapedRun run = ScriptedTextBackend::shapeRun(font, fontSizePx, spanText, byteOffset,
                                                  byteLength, isVertical, fontVariant,
                                                  forceLogicalOrder);
    for (auto& glyph : run.glyphs) {
      const auto [codepoint, codepointLength] =
          Utf8::NextCodepointLenient(spanText.substr(glyph.cluster));
      (void)codepointLength;
      if (static_cast<uint32_t>(codepoint) > 0x7F && font != coveringFont) {
        glyph.glyphIndex = 0;
      }
    }
    return run;
  }
};

/// Backend whose font reports no vertical metrics, forcing the inline-size wrap to fall
/// back to the 1.2 * font-size line height.
class ZeroVMetricsBackend : public ScriptedTextBackend {
public:
  FontVMetrics fontVMetrics(FontHandle /*font*/) const override { return {}; }
};

/// Backend with no vector outlines: 'A' resolves to a bitmap glyph, everything else to a
/// zero-sized bitmap (empty extent).
class BitmapOutlineBackend : public ScriptedTextBackend {
public:
  Path glyphOutline(FontHandle /*font*/, int /*glyphIndex*/, float /*scale*/) const override {
    return Path();
  }

  std::optional<BitmapGlyph> bitmapGlyph(FontHandle /*font*/, int glyphIndex,
                                         float /*scale*/) const override {
    if (glyphIndex == 'A' % 997 + 1) {
      return BitmapGlyph{.rgbaPixels = {},
                         .width = 4,
                         .height = 4,
                         .bearingX = 1.0,
                         .bearingY = 2.0,
                         .scale = 0.5};
    }
    return BitmapGlyph{
        .rgbaPixels = {}, .width = 0, .height = 0, .bearingX = 0.0, .bearingY = 0.0, .scale = 1.0};
  }
};

/// Backend whose outlines contain quadratic and cubic curve segments.
class CurveOutlineBackend : public ScriptedTextBackend {
public:
  Path glyphOutline(FontHandle /*font*/, int /*glyphIndex*/, float /*scale*/) const override {
    return PathBuilder()
        .moveTo(Vector2d(0.0, 0.0))
        .quadTo(Vector2d(5.0, -10.0), Vector2d(10.0, 0.0))
        .curveTo(Vector2d(12.0, 5.0), Vector2d(2.0, 5.0), Vector2d(0.0, 10.0))
        .build();
  }
};

/// Registers a @font-face named \p familyName backed by the embedded fallback font's bytes,
/// returning its resolved handle.
FontHandle RegisterFallbackBackedFace(FontManager& fontManager, const std::string& familyName) {
  const FontHandle fallback = fontManager.fallbackFont();
  const auto data = fontManager.fontData(fallback);
  auto payload = std::make_shared<const std::vector<uint8_t>>(data.begin(), data.end());

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = payload;

  css::FontFace face;
  face.familyName = RcString(familyName);
  face.sources.push_back(std::move(source));
  fontManager.addFontFace(face);
  return fontManager.findFont(RcString(familyName));
}

/// Minimal sfnt bytes that pass the outline-table check but fail stb_truetype parsing
/// (no cmap table), so shaping produces no glyphs.
std::shared_ptr<const std::vector<uint8_t>> MakeUnparseableOutlineFontData() {
  std::vector<uint8_t> data = {0x00, 0x01, 0x00, 0x00,  // sfnt version 1.0
                               0x00, 0x01,              // numTables = 1
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const char tag[4] = {'g', 'l', 'y', 'f'};
  data.insert(data.end(), tag, tag + 4);
  for (int i = 0; i < 4; ++i) data.push_back(0);         // Checksum.
  data.insert(data.end(), {0x00, 0x00, 0x00, 0x1C});     // Offset = 28.
  for (int i = 0; i < 4; ++i) data.push_back(0);         // Length = 0.
  return std::make_shared<const std::vector<uint8_t>>(std::move(data));
}

components::ComputedStyleComponent MakeStyle() {
  components::ComputedStyleComponent style;
  style.properties.emplace();
  return style;
}

/// Creates a text root entity with computed text/style/viewbox components and a single
/// span sourced from the root, ready for the cached-geometry APIs.
Entity MakeTextRootWithSpan(Registry& registry, const std::string& textValue,
                            components::ComputedTextComponent::TextSpan span) {
  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);
  registry.emplace<components::ComputedViewBoxComponent>(root,
                                                         Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));

  components::TextComponent textComponent;
  textComponent.text = RcString(textValue);
  registry.emplace<components::TextComponent>(root, textComponent);

  span.sourceEntity = root;
  components::ComputedTextComponent computedText;
  computedText.spans.push_back(std::move(span));
  registry.emplace<components::ComputedTextComponent>(root, computedText);

  registry.emplace<components::ComputedStyleComponent>(root, MakeStyle());
  return root;
}

}  // namespace

TEST(TextEngineScriptedTest, PrepareForElementIgnoresElementsWithoutTextRoot) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  ParseWarningSink warnings;

  // Entity with no TreeComponent: the text-root walk stops immediately.
  const Entity detached = registry.create();
  engine.prepareForElement(EntityHandle(registry, detached), warnings);

  // Entity in a tree without any TextRootComponent ancestor.
  const Entity group = registry.create();
  registry.emplace<donner::components::TreeComponent>(group, xml::XMLQualifiedNameRef("g"));
  engine.prepareForElement(EntityHandle(registry, group), warnings);

  EXPECT_FALSE(registry.ctx().contains<FontManager>());
  EXPECT_EQ(registry.try_get<components::ComputedTextComponent>(group), nullptr);
}

TEST(TextEngineScriptedTest, PrepareForElementRequiresResourceManagerContext) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  ParseWarningSink warnings;

  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);

  engine.prepareForElement(EntityHandle(registry, root), warnings);

  // Without a ResourceManagerContext the engine bails out before installing anything.
  EXPECT_FALSE(registry.ctx().contains<FontManager>());
  EXPECT_EQ(registry.try_get<components::ComputedTextComponent>(root), nullptr);
}

TEST(TextEngineScriptedTest, AddFontFaceRegistersSingleFaceAndBatchSkipsAlreadyRegistered) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  css::FontFace face;
  face.familyName = RcString("SoloFace");
  engine.addFontFace(face);
  EXPECT_EQ(fontManager.numFaces(), 1u);

  // A batch no larger than the registered count is a no-op.
  const std::vector<css::FontFace> faces = {face};
  engine.addFontFaces(faces);
  EXPECT_EQ(fontManager.numFaces(), 1u);
}

TEST(TextEngineScriptedTest, ResolvePerSpanLayoutStylesRequiresRootTextComponent) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);
  auto rootStyle = MakeStyle();
  rootStyle.properties->textAnchor.set(TextAnchor::End, css::Specificity::Override());
  registry.emplace<components::ComputedStyleComponent>(root, rootStyle);

  components::ComputedTextComponent text;
  auto span = MakeSpan("A");
  span.sourceEntity = root;
  text.spans.push_back(std::move(span));

  // No TextComponent on the root: resolution is skipped and spans stay untouched.
  engine.resolvePerSpanLayoutStyles(EntityHandle(registry, root), text);
  EXPECT_EQ(text.spans[0].textAnchor, TextAnchor::Start);
}

TEST(TextEngineScriptedTest, ResolvePerSpanLayoutStylesFallsBackToParentStyleAndSkipsUnstyled) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = MakeTextRootWithSpan(registry, "ABCD", MakeSpan("A"));

  // A styled tspan parent whose child has no computed style of its own.
  const Entity styledParent = registry.create();
  registry.emplace<donner::components::TreeComponent>(styledParent,
                                                      xml::XMLQualifiedNameRef("tspan"));
  registry.get<donner::components::TreeComponent>(root).appendChild(registry, styledParent);
  auto parentStyle = MakeStyle();
  parentStyle.properties->textAnchor.set(TextAnchor::End, css::Specificity::Override());
  registry.emplace<components::ComputedStyleComponent>(styledParent, parentStyle);

  const Entity unstyledChild = registry.create();
  registry.emplace<donner::components::TreeComponent>(unstyledChild,
                                                      xml::XMLQualifiedNameRef("tspan"));
  registry.get<donner::components::TreeComponent>(styledParent)
      .appendChild(registry, unstyledChild);

  const Entity orphanNoTree = registry.create();
  const Entity orphanWithTree = registry.create();
  registry.emplace<donner::components::TreeComponent>(orphanWithTree,
                                                      xml::XMLQualifiedNameRef("tspan"));

  components::ComputedTextComponent text;
  auto nullSourceSpan = MakeSpan("A");
  nullSourceSpan.sourceEntity = entt::null;
  auto orphanSpan = MakeSpan("B");
  orphanSpan.sourceEntity = orphanNoTree;
  auto orphanTreeSpan = MakeSpan("C");
  orphanTreeSpan.sourceEntity = orphanWithTree;
  auto childSpan = MakeSpan("D");
  childSpan.sourceEntity = unstyledChild;
  text.spans.push_back(std::move(nullSourceSpan));
  text.spans.push_back(std::move(orphanSpan));
  text.spans.push_back(std::move(orphanTreeSpan));
  text.spans.push_back(std::move(childSpan));

  engine.resolvePerSpanLayoutStyles(EntityHandle(registry, root), text);

  // Spans without a resolvable style source stay at their defaults.
  EXPECT_EQ(text.spans[0].textAnchor, TextAnchor::Start);
  EXPECT_EQ(text.spans[1].textAnchor, TextAnchor::Start);
  EXPECT_EQ(text.spans[2].textAnchor, TextAnchor::Start);
  // A span without its own style resolves through its parent element's style.
  EXPECT_EQ(text.spans[3].textAnchor, TextAnchor::End);
}

TEST(TextEngineScriptedTest, ResolvePerSpanLayoutStylesResolvesKeywordsAncestorsAndTextLength) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = MakeTextRootWithSpan(registry, "ABX", MakeSpan("X"));
  auto& rootStyle = registry.get<components::ComputedStyleComponent>(root);
  rootStyle.properties->baselineShift.set(Lengthd(5.0, Lengthd::Unit::Px),
                                          css::Specificity::Override());

  auto makeChild = [&](Entity parent, std::optional<Lengthd> baselineShift) {
    const Entity entity = registry.create();
    registry.emplace<donner::components::TreeComponent>(entity,
                                                        xml::XMLQualifiedNameRef("tspan"));
    registry.get<donner::components::TreeComponent>(parent).appendChild(registry, entity);
    if (baselineShift.has_value()) {
      auto style = MakeStyle();
      style.properties->baselineShift.set(*baselineShift, css::Specificity::Override());
      registry.emplace<components::ComputedStyleComponent>(entity, style);
    }
    return entity;
  };

  // Chain: root -> super(0.4em) -> sub(-0.33em) -> unstyled -> length(3) -> leaf(-0.33em).
  const Entity superAncestor = makeChild(root, Lengthd(0.4, Lengthd::Unit::Em));
  const Entity subAncestor = makeChild(superAncestor, Lengthd(-0.33, Lengthd::Unit::Em));
  const Entity unstyledAncestor = makeChild(subAncestor, std::nullopt);
  const Entity lengthAncestor = makeChild(unstyledAncestor, Lengthd(3.0, Lengthd::Unit::None));
  const Entity leaf = makeChild(lengthAncestor, Lengthd(-0.33, Lengthd::Unit::Em));

  // dominant-baseline: no-change on the leaf resolves to the parent's value.
  registry.get<components::ComputedStyleComponent>(leaf).properties->dominantBaseline.set(
      DominantBaseline::NoChange, css::Specificity::Override());
  registry.get<components::ComputedStyleComponent>(lengthAncestor)
      .properties->dominantBaseline.set(DominantBaseline::Hanging, css::Specificity::Override());

  // Per-span textLength comes from the source element's TextComponent.
  components::TextComponent leafText;
  leafText.text = RcString("A");
  leafText.textLength = Lengthd(50.0, Lengthd::Unit::None);
  leafText.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  registry.emplace<components::TextComponent>(leaf, leafText);

  const Entity superLeaf = makeChild(root, Lengthd(0.4, Lengthd::Unit::Em));

  components::ComputedTextComponent text;
  auto leafSpan = MakeSpan("A");
  leafSpan.sourceEntity = leaf;
  auto rootSpan = MakeSpan("B");
  rootSpan.sourceEntity = root;
  auto superSpan = MakeSpan("X");
  superSpan.sourceEntity = superLeaf;
  text.spans.push_back(std::move(leafSpan));
  text.spans.push_back(std::move(rootSpan));
  text.spans.push_back(std::move(superSpan));

  engine.resolvePerSpanLayoutStyles(EntityHandle(registry, root), text);

  const auto& resolvedLeaf = text.spans[0];
  EXPECT_EQ(resolvedLeaf.baselineShiftKeyword, BSK::Sub);
  EXPECT_EQ(resolvedLeaf.alignmentBaseline, DominantBaseline::Hanging);
  ASSERT_TRUE(resolvedLeaf.textLength.has_value());
  EXPECT_DOUBLE_EQ(resolvedLeaf.textLength->value, 50.0);
  EXPECT_EQ(resolvedLeaf.lengthAdjust, LengthAdjust::SpacingAndGlyphs);

  // Ancestors accumulate bottom-up, skipping the unstyled element.
  ASSERT_EQ(resolvedLeaf.ancestorBaselineShifts.size(), 3u);
  EXPECT_EQ(resolvedLeaf.ancestorBaselineShifts[0].keyword, BSK::Length);
  EXPECT_DOUBLE_EQ(resolvedLeaf.ancestorBaselineShifts[0].shift.value, 3.0);
  EXPECT_EQ(resolvedLeaf.ancestorBaselineShifts[1].keyword, BSK::Sub);
  EXPECT_EQ(resolvedLeaf.ancestorBaselineShifts[2].keyword, BSK::Super);

  // Spans sourced from the text root ignore baseline-shift entirely.
  EXPECT_DOUBLE_EQ(text.spans[1].baselineShift.value, 0.0);
  EXPECT_TRUE(text.spans[1].ancestorBaselineShifts.empty());

  EXPECT_EQ(text.spans[2].baselineShiftKeyword, BSK::Super);
}

TEST(TextEngineScriptedTest, CoverageFallbackSwitchesToRegisteredFaceThatCoversCodepoint) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle altFace = RegisterFallbackBackedFace(fontManager, "AltFace");
  ASSERT_TRUE(static_cast<bool>(altFace));

  auto backend = std::make_unique<CoverageProbeBackend>();
  backend->coveringFont = altFace;
  TextEngine engine(fontManager, registry, std::move(backend));

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("\xC3\xA9"));  // e-acute: not covered by the base font.

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(1))));
  EXPECT_EQ(runs[0].font, altFace);
  EXPECT_THAT(runs[0].glyphs, ElementsAre(GlyphIndexIs(Gt(0))));
}

TEST(TextEngineScriptedTest, CoverageFallbackKeepsFontWhenNoRegisteredFaceCovers) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle altFace = RegisterFallbackBackedFace(fontManager, "AltFace");
  ASSERT_TRUE(static_cast<bool>(altFace));

  // No font covers non-ASCII: the search visits the registered face and keeps the original.
  auto backend = std::make_unique<CoverageProbeBackend>();
  TextEngine engine(fontManager, registry, std::move(backend));

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("\xC3\xA9"));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(1))));
  EXPECT_EQ(runs[0].font, fontManager.fallbackFont());
  EXPECT_THAT(runs[0].glyphs, ElementsAre(GlyphIndexIs(Eq(0))));
}

TEST(TextEngineScriptedTest, BoldSpanResolvesFontThroughWeightAwareLookup) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle altFace = RegisterFallbackBackedFace(fontManager, "WeightProbe");
  ASSERT_TRUE(static_cast<bool>(altFace));
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("A");
  span.fontWeight = 700;
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(20.0);
  params.fontFamilies = {RcString("WeightProbe")};
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(1))));
  EXPECT_EQ(runs[0].font, altFace);
}

TEST(TextEngineScriptedTest, HorizontalCrossSpanKernShiftsContinuationSpan) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));
  auto continuation = MakeSpan("B");
  continuation.startsNewChunk = false;
  text.spans.push_back(std::move(continuation));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  // The continuation span's first glyph picks up the scripted 3px cross-span kern.
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(0.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(13.0))))));
}

TEST(TextEngineScriptedTest, VerticalCrossSpanKernShiftsContinuationSpan) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));
  auto continuation = MakeSpan("B");
  continuation.startsNewChunk = false;
  text.spans.push_back(std::move(continuation));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  // 10px sideways advance for "A", then the scripted 4px vertical cross-span kern.
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(0.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(14.0))))));
}

TEST(TextEngineScriptedTest, VerticalCjkRotateListRepeatsLastValue) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("\xE6\x97\xA5\xE6\x97\xA5");  // Two upright CJK glyphs.
  span.rotateList = {7.0};
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  // The single rotate value repeats for the second glyph per the SVG spec.
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphRotateDegreesIs(DoubleEq(7.0)),
                              GlyphYPositionIs(DoubleEq(3.0))),
                        AllOf(GlyphRotateDegreesIs(DoubleEq(7.0)),
                              GlyphYPositionIs(DoubleEq(17.0)))))));
}

TEST(TextEngineScriptedTest, PerSpanTextLengthSpacingAdjustsVerticalGaps) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.textLength = Lengthd(40.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::Spacing;
  text.spans.push_back(std::move(span));

  // A sibling span without textLength is skipped by the per-span adjustment.
  auto plainSpan = MakeSpan("C");
  plainSpan.startsNewChunk = false;
  text.spans.push_back(std::move(plainSpan));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  // Natural length 22px (10px advance, 2px kern, 10px advance) stretched to 40px moves
  // the second glyph down by the 18px gap difference. The plain span keeps its natural
  // position after the first span's unadjusted 22px extent plus the 4px cross-span kern.
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(0.0)),
                                                         GlyphYPositionIs(DoubleEq(30.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(26.0))))));
}

TEST(TextEngineScriptedTest, InlineSizeDoesNotWrapTextOnPath) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const entt::entity textPathEntity = registry.create();
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(400.0, 0.0)).build();

  components::ComputedTextComponent text;
  auto span = MakeSpan("aa bb");
  span.pathSpline = path;
  span.textPathSourceEntity = textPathEntity;
  text.spans.push_back(std::move(span));

  TextLayoutParams params = MakeTextParams(20.0);
  params.inlineSizePx = 25.0;  // Would wrap flat text; ignored for text-on-path.
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(5))));
  EXPECT_TRUE(runs[0].onPath);
  for (const auto& glyph : runs[0].glyphs) {
    EXPECT_THAT(glyph.yPosition, DoubleNear(0.0, 1e-9));  // No stacked lines.
  }
}

TEST(TextEngineScriptedTest, InlineSizeIgnoresSingleGlyphContent) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("a"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.inlineSizePx = 5.0;  // Smaller than the glyph; still nothing to wrap.
  const auto runs = engine.layout(text, params);

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(AllOf(
                        GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(0.0)))))));
}

TEST(TextEngineScriptedTest, InlineSizeLineHeightFallsBackWhenFontMetricsAreEmpty) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry, std::make_unique<ZeroVMetricsBackend>());

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("aa bb"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.inlineSizePx = 25.0;  // Each two-glyph word (21px) fits alone; both do not.
  const auto runs = engine.layout(text, params);

  std::vector<long> baselines;
  for (const auto& run : runs) {
    for (const auto& glyph : run.glyphs) {
      baselines.push_back(std::lround(glyph.yPosition));
    }
  }
  std::sort(baselines.begin(), baselines.end());
  baselines.erase(std::unique(baselines.begin(), baselines.end()), baselines.end());

  // Zero font metrics force the 1.2 * font-size (24px) line-height fallback.
  EXPECT_THAT(baselines, ElementsAre(0, 24));
}

TEST(TextEngineScriptedTest, TextAnchorSkipsOnPathRunsInsideChunks) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const entt::entity firstPathEntity = registry.create();
  const entt::entity secondPathEntity = registry.create();
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();

  components::ComputedTextComponent text;
  auto flatSpan = MakeSpan("AB");
  flatSpan.textAnchor = TextAnchor::End;

  auto onPathSpan = MakeSpan("C");
  onPathSpan.startsNewChunk = false;
  onPathSpan.pathSpline = path;
  onPathSpan.textPathSourceEntity = firstPathEntity;

  // Second chunk: an empty span (anchor end) followed by on-path glyphs only, so the
  // chunk has no flat glyphs to anchor.
  auto emptySpan = MakeSpan("");
  emptySpan.textAnchor = TextAnchor::End;

  auto onPathTail = MakeSpan("D");
  onPathTail.startsNewChunk = false;
  onPathTail.textAnchor = TextAnchor::End;
  onPathTail.pathSpline = path;
  onPathTail.textPathSourceEntity = secondPathEntity;
  onPathTail.pathStartOffset = 80.0;

  text.spans.push_back(std::move(flatSpan));
  text.spans.push_back(std::move(onPathSpan));
  text.spans.push_back(std::move(emptySpan));
  text.spans.push_back(std::move(onPathTail));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  // Flat glyphs shift by the chunk width (21px); on-path glyphs stay path-positioned.
  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(-21.0)),
                                                   GlyphXPositionIs(DoubleEq(-10.0)))),
                          RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(0.0)))),
                          RunGlyphsAre(IsEmpty()),
                          RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(70.0))))));
}

TEST(TextEngineScriptedTest, GeometryCacheFallsBackToBitmapGlyphExtents) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry, std::make_unique<BitmapOutlineBackend>());

  const Entity root = MakeTextRootWithSpan(registry, "AB", MakeSpan("AB"));
  const EntityHandle rootHandle(registry, root);

  // No vector outlines anywhere.
  EXPECT_THAT(engine.computedGlyphPaths(rootHandle), IsEmpty());

  // 'A' resolves to a 4x4 bitmap at scale 0.5 with bearing (1, 2).
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 0), Box2d::FromXYWH(1.0, -2.0, 2.0, 2.0));
  // 'B' resolves to a zero-sized bitmap: its extent stays empty.
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 1), Box2d());

  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 2);
  // End position advances by the scripted (10, 12) glyph advance.
  EXPECT_EQ(engine.getEndPositionOfChar(rootHandle, 0), Vector2d(10.0, 12.0));

  // Out-of-range character queries return empty values.
  EXPECT_EQ(engine.getEndPositionOfChar(rootHandle, 9), Vector2d());
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 9), Box2d());
  EXPECT_DOUBLE_EQ(engine.getRotationOfChar(rootHandle, 9), 0.0);
}

TEST(TextEngineScriptedTest, GeometryCacheTransformsQuadraticAndCubicOutlineSegments) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine(fontManager, registry, std::make_unique<CurveOutlineBackend>());

  auto span = MakeSpan("A");
  span.xList = {Lengthd(7.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(9.0, Lengthd::Unit::None)};
  const Entity root = MakeTextRootWithSpan(registry, "A", std::move(span));

  const std::vector<Path> paths = engine.computedGlyphPaths(EntityHandle(registry, root));

  // The outline's move/quad/cubic control points all translate by the glyph position.
  ASSERT_THAT(paths, SizeIs(1));
  EXPECT_THAT(paths[0].points(),
              ElementsAre(Vector2d(7.0, 9.0), Vector2d(12.0, -1.0), Vector2d(17.0, 9.0),
                          Vector2d(19.0, 14.0), Vector2d(9.0, 14.0), Vector2d(7.0, 19.0)));
}

TEST(TextEngineScriptedTest, GeometryCacheFilterExcludesEntitiesOutsideTree) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const Entity root = MakeTextRootWithSpan(registry, "A", MakeSpan("A"));
  const EntityHandle rootHandle(registry, root);
  ASSERT_EQ(engine.getNumberOfChars(rootHandle), 1);  // Builds the geometry cache.

  // Inject a cached glyph whose source entity has no TreeComponent: the descendant
  // filter cannot walk it and excludes it from subtree queries.
  const Entity detached = registry.create();
  auto& cache = registry.get<components::ComputedTextGeometryComponent>(root);
  cache.glyphs.push_back(components::ComputedTextGeometryComponent::GlyphGeometry{
      .sourceEntity = detached,
      .path = PathBuilder().addRect(Box2d::FromXYWH(50.0, 0.0, 5.0, 5.0)).build(),
      .extent = Box2d::FromXYWH(50.0, 0.0, 5.0, 5.0),
  });

  const auto paths = engine.computedGlyphPaths(rootHandle);
  EXPECT_THAT(paths, SizeIs(1));  // Only the root-sourced glyph; the detached one is filtered.

  // The outline+source variant applies the same subtree filter.
  const auto outlines = engine.computedGlyphOutlines(rootHandle);
  ASSERT_THAT(outlines, SizeIs(1));
  EXPECT_EQ(outlines[0].sourceEntity, root);
}

TEST(TextEngineScriptedTest, MetricForwardersExposeBackendValues) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const FontHandle font;

  EXPECT_EQ(engine.fontVMetrics(font).ascent, 1000);
  EXPECT_FLOAT_EQ(engine.scaleForPixelHeight(font, 24.0f), 0.02f);
  EXPECT_FLOAT_EQ(engine.scaleForEmToPixels(font, 24.0f), 0.024f);

  const auto underline = engine.underlineMetrics(font);
  ASSERT_TRUE(underline.has_value());
  EXPECT_DOUBLE_EQ(underline->position, -100.0);
  EXPECT_DOUBLE_EQ(underline->thickness, 50.0);

  const auto strikeout = engine.strikeoutMetrics(font);
  ASSERT_TRUE(strikeout.has_value());
  EXPECT_DOUBLE_EQ(strikeout->position, 300.0);
  EXPECT_DOUBLE_EQ(strikeout->thickness, 40.0);

  EXPECT_FALSE(engine.subSuperMetrics(font).has_value());
  EXPECT_FALSE(engine.isBitmapOnly(font));
  EXPECT_FALSE(engine.bitmapGlyph(font, 3, 1.0f).has_value());
  EXPECT_FALSE(engine.glyphOutline(font, 5, 1.0f).empty());
  EXPECT_TRUE(engine.glyphOutline(font, 0, 1.0f).empty());
}

TEST(TextEngineScriptedTest, MeasureChUnitUsesFallbackFontWhenNoFamilyMatches) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const std::optional<double> chUnit = engine.measureChUnitInEm({});

  // The embedded fallback font's '0' advance, in ems.
  ASSERT_TRUE(chUnit.has_value());
  EXPECT_THAT(*chUnit, AllOf(Gt(0.0), testing::Lt(2.0)));
}

TEST(TextEngineScriptedTest, MeasureChUnitFailsWhenFontCannotBeShaped) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  // Register a face whose data passes the outline check but cannot be parsed for shaping.
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = MakeUnparseableOutlineFontData();
  css::FontFace face;
  face.familyName = RcString("BrokenFont");
  face.sources.push_back(std::move(source));
  fontManager.addFontFace(face);

  const std::vector<RcString> families = {RcString("BrokenFont")};
  EXPECT_FALSE(engine.measureChUnitInEm(families).has_value());
}

}  // namespace donner::svg
