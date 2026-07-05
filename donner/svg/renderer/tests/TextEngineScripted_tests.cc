#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Utf8.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextEngine.h"
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
  EXPECT_TRUE(engine.computedGlyphPaths(rootHandle).empty());
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
  EXPECT_EQ(glyphPaths.size(), 3u);
  EXPECT_GT(engine.computedInkBounds(rootHandle).width(), 0.0);
  EXPECT_GT(engine.computedObjectBoundingBox(rootHandle).height(), 0.0);
  EXPECT_GT(engine.getComputedTextLength(rootHandle), 0.0);
  EXPECT_GT(engine.getSubStringLength(rootHandle, 0, 1), 0.0);
  EXPECT_EQ(engine.getStartPositionOfChar(rootHandle, 2), Vector2d());
  EXPECT_NE(engine.getRotationOfChar(rootHandle, 0), 0.0);
  EXPECT_TRUE(engine.getExtentOfChar(rootHandle, 0).width() > 0.0);
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

  ASSERT_EQ(runs.size(), 2u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);
  ASSERT_EQ(runs[1].glyphs.size(), 3u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 11.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 22.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].xPosition, 24.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[1].xPosition, 38.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[2].xPosition, 82.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[2].yPosition, 44.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[0].rotateDegrees, 5.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[1].rotateDegrees, 15.0);
  EXPECT_DOUBLE_EQ(runs[1].glyphs[2].rotateDegrees, 15.0);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 3u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 0.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 13.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[2].xPosition, 70.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[2].yPosition, 90.0);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 70.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 90.0);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 3u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].rotateDegrees, 97.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].rotateDegrees, 17.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[2].rotateDegrees, 107.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 114.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 47.0);
  EXPECT_GT(runs[0].glyphs[2].yPosition, runs[0].glyphs[1].yPosition);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 4u);
  EXPECT_GT(runs[0].glyphs[1].xPosition, runs[0].glyphs[0].xPosition);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 20.0);
  EXPECT_GT(runs[0].glyphs[3].xPosition, runs[0].glyphs[1].xPosition);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[3].yPosition, 70.0);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 4u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[2].yPosition, 60.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[3].yPosition, 60.0);
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

  ASSERT_EQ(runs.size(), 2u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  ASSERT_EQ(runs[1].glyphs.size(), 2u);
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, -10.5, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 0.5, 0.001);
  EXPECT_NEAR(runs[1].glyphs[0].xPosition, 29.0, 0.001);
  EXPECT_NEAR(runs[1].glyphs[1].xPosition, 40.0, 0.001);
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

  ASSERT_EQ(runs.size(), 3u);
  EXPECT_TRUE(runs[0].glyphs.empty());
  EXPECT_TRUE(runs[1].glyphs.empty());
  ASSERT_EQ(runs[2].glyphs.size(), 1u);
  EXPECT_DOUBLE_EQ(runs[2].glyphs[0].xPosition, 0.0);
  EXPECT_DOUBLE_EQ(runs[2].glyphs[0].yPosition, 0.0);
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

  ASSERT_EQ(runs.size(), 3u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  ASSERT_EQ(runs[1].glyphs.size(), 1u);
  EXPECT_TRUE(runs[0].onPath);
  EXPECT_TRUE(runs[1].onPath);
  EXPECT_TRUE(runs[2].onPath);
  EXPECT_NEAR(runs[0].glyphs[0].xPosition, -0.5, 0.001);
  EXPECT_NEAR(runs[0].glyphs[1].xPosition, 10.5, 0.001);
  EXPECT_NEAR(runs[1].glyphs[0].xPosition, 20.5, 0.001);
  EXPECT_TRUE(runs[2].glyphs.empty());
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 3u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xPosition, 0.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xPosition, 22.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[2].xPosition, 44.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].xAdvance, 20.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].xAdvance, 20.0);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].stretchScaleX, 2.0f);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 0.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 34.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yAdvance, 10.0);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].stretchScaleY, 1.0f);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  const TextGlyph& base = runs[0].glyphs[0];
  const TextGlyph& mark = runs[0].glyphs[1];
  EXPECT_NEAR(mark.xPosition, base.xPosition, 1e-6);
  EXPECT_NEAR(mark.yPosition, base.yPosition + 11.0, 1e-6);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yPosition, 0.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[1].yPosition, 24.0);
  EXPECT_DOUBLE_EQ(runs[0].glyphs[0].yAdvance, 20.0);
  EXPECT_FLOAT_EQ(runs[0].glyphs[0].stretchScaleY, 2.0f);
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

  ASSERT_EQ(runs.size(), 3u);
  ASSERT_EQ(runs[0].glyphs.size(), 1u);
  ASSERT_EQ(runs[1].glyphs.size(), 1u);
  ASSERT_EQ(runs[2].glyphs.size(), 1u);
  EXPECT_NEAR(runs[0].glyphs[0].yPosition, 6.0, 1e-6);
  EXPECT_NEAR(runs[1].glyphs[0].yPosition, -10.0, 1e-6);
  EXPECT_NEAR(runs[2].glyphs[0].yPosition, -4.0, 1e-6);
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

  ASSERT_EQ(runs.size(), 1u);
  ASSERT_EQ(runs[0].glyphs.size(), 2u);
  EXPECT_TRUE(runs[0].onPath);
  EXPECT_EQ(runs[0].glyphs[0].glyphIndex, 0);
  EXPECT_EQ(runs[0].glyphs[1].glyphIndex, 0);
}

}  // namespace donner::svg
