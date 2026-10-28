#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "donner/base/Utf8.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/FontFace.h"
#include "donner/css/Specificity.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
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

class ScriptedTextBackend : public TextBackend {
public:
  bool reverseClusters = false;
  bool nativeSmallCaps = false;
  bool clusterCombiningMarks = false;
  std::optional<SubSuperMetrics> subSuper;
  std::shared_ptr<float> shapedFontSizePx;
  int xHeight = 500;
  mutable std::size_t outlineCalls = 0;
  mutable std::optional<FontVariant> lastShapedVariant;

  FontVMetrics fontVMetrics(FontHandle /*font*/) const override {
    return FontVMetrics{
        .ascent = 1000,
        .descent = -200,
        .lineGap = 0,
        .xHeight = xHeight,
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
    return subSuper;
  }

  Path glyphOutline(FontHandle /*font*/, int glyphIndex, float /*scale*/) const override {
    ++outlineCalls;
    if (glyphIndex == 0) {
      return Path();
    }
    return PathBuilder().addRect(Box2d::FromXYWH(0.0, -8.0, 6.0, 8.0)).build();
  }

  bool isBitmapOnly(FontHandle /*font*/) const override { return false; }

  bool isCursive(uint32_t codepoint) const override { return codepoint == U'c'; }

  bool hasSmallCapsFeature(FontHandle /*font*/) const override { return nativeSmallCaps; }

  std::optional<BitmapGlyph> bitmapGlyph(FontHandle /*font*/, int /*glyphIndex*/,
                                         float /*scale*/) const override {
    return std::nullopt;
  }

  ShapedRun shapeRun(FontHandle /*font*/, float fontSizePx, std::string_view spanText,
                     size_t byteOffset, size_t byteLength, bool /*isVertical*/,
                     FontVariant fontVariant, bool forceLogicalOrder) const override {
    lastShapedVariant = fontVariant;
    if (shapedFontSizePx) {
      *shapedFontSizePx = fontSizePx;
    }
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
      applyCombiningMarkPosition(glyph, run, codepoint);
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

  void applyCombiningMarkPosition(ShapedGlyph& glyph, const ShapedRun& run,
                                  uint32_t codepoint) const {
    if (clusterCombiningMarks && codepoint == 0x0301 && !run.glyphs.empty()) {
      glyph.cluster = run.glyphs.back().cluster;
      glyph.xKern = 0.0;
      glyph.yKern = 0.0;
      glyph.xOffset = -7.0;
      glyph.yOffset = -4.0;
    }
  }

  ShapedRun shapeRunNoKerning(FontHandle font, float fontSizePx, std::string_view spanText,
                              size_t byteOffset, size_t byteLength, bool isVertical,
                              FontVariant fontVariant, bool forceLogicalOrder) const override {
    ShapedRun run = shapeRun(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                             fontVariant, forceLogicalOrder);
    for (auto& glyph : run.glyphs) {
      glyph.xKern = 0.0;
      glyph.yKern = 0.0;
    }
    return run;
  }

  double crossSpanKern(FontHandle /*prevFont*/, float /*prevSizePx*/, FontHandle /*curFont*/,
                       float /*curSizePx*/, uint32_t /*prevCodepoint*/, uint32_t /*curCodepoint*/,
                       bool isVertical) const override {
    return isVertical ? 4.0 : 3.0;
  }
};

auto BitmapBoundsNear(const Box2d& expected) {
  return AllOf(Field("topLeft", &Box2d::topLeft,
                     AllOf(Field("x", &Vector2d::x, DoubleNear(expected.topLeft.x, 1e-9)),
                           Field("y", &Vector2d::y, DoubleNear(expected.topLeft.y, 1e-9)))),
               Field("bottomRight", &Box2d::bottomRight,
                     AllOf(Field("x", &Vector2d::x, DoubleNear(expected.bottomRight.x, 1e-9)),
                           Field("y", &Vector2d::y, DoubleNear(expected.bottomRight.y, 1e-9)))));
}

TextEngine MakeScriptedEngine(Registry& registry, FontManager& fontManager,
                              bool reverseClusters = false,
                              std::optional<SubSuperMetrics> subSuper = std::nullopt,
                              bool clusterCombiningMarks = false) {
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->reverseClusters = reverseClusters;
  backend->clusterCombiningMarks = clusterCombiningMarks;
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

namespace {

EntityHandle MakeReferenceTextRoot(Registry& registry, Visibility visibility) {
  const Entity root = registry.create();
  registry.emplace<donner::components::TreeComponent>(root, xml::XMLQualifiedNameRef("text"));
  registry.emplace<components::TextRootComponent>(root);
  registry.emplace<components::ComputedViewBoxComponent>(root, Box2d::FromXYWH(0, 0, 200, 100));
  components::TextComponent text;
  text.text = RcString("A");
  registry.emplace<components::TextComponent>(root, text);
  components::ComputedTextComponent computed;
  auto span = MakeSpan("A");
  span.sourceEntity = root;
  span.xList = {Lengthd(10, Lengthd::Unit::None)};
  span.yList = {Lengthd(20, Lengthd::Unit::None)};
  computed.spans.push_back(std::move(span));
  registry.emplace<components::ComputedTextComponent>(root, std::move(computed));
  components::ComputedStyleComponent style;
  style.properties.emplace();
  style.properties->fontSize.set(Lengthd(12, Lengthd::Unit::Px), css::Specificity::Override());
  style.properties->visibility.set(visibility, css::Specificity::Override());
  registry.emplace<components::ComputedStyleComponent>(root, std::move(style));
  return EntityHandle(registry, root);
}

EntityHandle MakeStretchedReferenceTextRoot(Registry& registry, Visibility visibility) {
  EntityHandle root = MakeReferenceTextRoot(registry, visibility);
  auto& span = root.get<components::ComputedTextComponent>().spans.front();
  span.xList.clear();
  span.yList.clear();
  span.pathSpline = PathBuilder()
                        .moveTo(Vector2d(0.0, 40.0))
                        .curveTo(Vector2d(0.0, 0.0), Vector2d(80.0, 0.0), Vector2d(80.0, 40.0))
                        .build();
  span.textPathSourceEntity = root.entity();
  span.textPathMethod = components::TextPathMethod::Stretch;
  span.rotateList = {15.0};
  return root;
}

}  // namespace

TEST(TextEngineScriptedTest, ReferenceFillBoundsIncludeHiddenCellsWithoutDecodingOutlines) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Hidden);
  std::size_t work = 100000;
  const auto geometry = engine.referenceGeometry(root, false, work);
  ASSERT_THAT(geometry, testing::Optional(testing::_));
  ASSERT_THAT(geometry->objectBounds, testing::Optional(testing::_));
  EXPECT_DOUBLE_EQ(geometry->objectBounds->topLeft.x, 10.0);
  EXPECT_NEAR(geometry->objectBounds->topLeft.y, 10.0, 1e-6);
  EXPECT_DOUBLE_EQ(geometry->objectBounds->bottomRight.x, 20.0);
  EXPECT_NEAR(geometry->objectBounds->bottomRight.y, 22.0, 1e-6);
  EXPECT_THAT(geometry->outlines, IsEmpty());
  EXPECT_EQ(probe->outlineCalls, 0u);
  EXPECT_THAT(root.all_of<components::ComputedTextGeometryComponent>(), testing::IsFalse());
}

std::optional<FontVariant> ShapeSmallCapsWithSynthesis(std::string_view policy,
                                                       bool nativeSmallCaps) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->nativeSmallCaps = nativeSmallCaps;
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Hidden);
  root.get<components::TextComponent>().text = RcString("a");
  root.get<components::ComputedTextComponent>().spans.front().text = RcString("a");
  root.get<components::ComputedStyleComponent>().properties->parseStyle(
      RcString("font-variant: small-caps; font-synthesis-small-caps: " + std::string(policy)));
  std::size_t work = 100000;
  EXPECT_THAT(engine.referenceGeometry(root, false, work), testing::Optional(testing::_));
  return probe->lastShapedVariant;
}

TEST(TextEngineScriptedTest, SmallCapsSynthesisNoneSuppressesOnlySyntheticCaps) {
  EXPECT_THAT(ShapeSmallCapsWithSynthesis("none", false), testing::Optional(FontVariant::Normal));
}

TEST(TextEngineScriptedTest, SmallCapsSynthesisAutoPreservesSyntheticCaps) {
  EXPECT_THAT(ShapeSmallCapsWithSynthesis("auto", false),
              testing::Optional(FontVariant::SmallCaps));
}

TEST(TextEngineScriptedTest, SmallCapsSynthesisNonePreservesNativeFeature) {
  EXPECT_THAT(ShapeSmallCapsWithSynthesis("none", true), testing::Optional(FontVariant::SmallCaps));
}

TEST(TextEngineScriptedTest, ReferenceSmallCapsCellsKeepFullFontHeight) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Hidden);
  root.get<components::TextComponent>().text = RcString("a");
  auto& span = root.get<components::ComputedTextComponent>().spans.front();
  span.text = RcString("a");
  span.fontVariant = FontVariant::SmallCaps;
  root.get<components::ComputedStyleComponent>().properties->fontVariant.set(
      FontVariant::SmallCaps, css::Specificity::Override());
  std::size_t work = 100000;

  const auto geometry = engine.referenceGeometry(root, false, work);

  ASSERT_THAT(probe->lastShapedVariant, testing::Optional(FontVariant::SmallCaps));
  ASSERT_THAT(geometry, testing::Optional(testing::_));
  // Synthetic small caps scale the ink by 0.8, not the element's 12px font cell.
  // The scripted font cell extends 10px above and 2px below its baseline at (10,20).
  EXPECT_THAT(geometry->objectBounds,
              testing::Optional(BitmapBoundsNear(Box2d::FromXYWH(10, 10, 10, 12))));
  EXPECT_THAT(geometry->outlines, IsEmpty());
  EXPECT_THAT(probe->outlineCalls, Eq(0u));
}

TEST(TextEngineScriptedTest, ShadowTextStyleContextIsBoundedAndRequiresSpanCoverage) {
  Registry registry;
  const Entity lightRoot = registry.create();
  const Entity lightSpan = registry.create();
  const Entity shadowRoot = registry.create();
  const Entity shadowSpan = registry.create();
  registry.emplace<donner::components::TreeComponent>(lightRoot, xml::XMLQualifiedNameRef("text"));
  registry.emplace<donner::components::TreeComponent>(lightSpan, xml::XMLQualifiedNameRef("tspan"));
  registry.emplace<donner::components::TreeComponent>(shadowRoot, xml::XMLQualifiedNameRef("text"));
  registry.emplace<donner::components::TreeComponent>(shadowSpan,
                                                      xml::XMLQualifiedNameRef("tspan"));
  registry.get<donner::components::TreeComponent>(shadowRoot).appendChild(registry, shadowSpan);
  registry.emplace<components::ShadowEntityComponent>(shadowRoot, lightRoot);
  registry.emplace<components::ShadowEntityComponent>(shadowSpan, lightSpan);
  for (int index = 1; index < 32; ++index) {
    const Entity extraLight = registry.create();
    const Entity extraShadow = registry.create();
    registry.emplace<donner::components::TreeComponent>(extraLight,
                                                        xml::XMLQualifiedNameRef("tspan"));
    registry.emplace<donner::components::TreeComponent>(extraShadow,
                                                        xml::XMLQualifiedNameRef("tspan"));
    registry.get<donner::components::TreeComponent>(shadowRoot).appendChild(registry, extraShadow);
    registry.emplace<components::ShadowEntityComponent>(extraShadow, extraLight);
  }
  components::ComputedTextComponent text;
  text.spans.emplace_back().sourceEntity = lightSpan;

  std::size_t insufficientWork = 1;
  EXPECT_THAT(BuildShadowTextStyleContext(registry, shadowRoot, text, &insufficientWork),
              Eq(std::nullopt));
  EXPECT_EQ(insufficientWork, 0u);

  std::size_t admittedWork = 67;
  const auto context = BuildShadowTextStyleContext(registry, shadowRoot, text, &admittedWork);
  ASSERT_THAT(context, testing::Optional(testing::_));
  EXPECT_EQ(context->rootEntity, shadowRoot);
  EXPECT_EQ(context->styleEntityFor(lightRoot), shadowRoot);
  EXPECT_EQ(context->styleEntityFor(lightSpan), shadowSpan);
  EXPECT_EQ(admittedWork, 0u);

  text.spans.front().sourceEntity = registry.create();
  std::size_t missingCoverageWork = 67;
  EXPECT_THAT(BuildShadowTextStyleContext(registry, shadowRoot, text, &missingCoverageWork),
              Eq(std::nullopt));
}

TEST(TextEngineScriptedTest, ReferenceStrokeOutlinesIncludeHiddenGlyphsWithoutChangingInkCache) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Hidden);
  std::size_t work = 100000;
  const auto geometry = engine.referenceGeometry(root, true, work);
  ASSERT_THAT(geometry, testing::Optional(testing::_));
  ASSERT_THAT(geometry->outlines, SizeIs(1));
  EXPECT_EQ(geometry->outlines[0].sourceEntity, root.entity());
  // The scripted outline is independently authored as [0,6] x [-8,0], placed at (10,20).
  EXPECT_THAT(geometry->outlines[0].path.bounds(), BitmapBoundsNear(Box2d::FromXYWH(10, 12, 6, 8)));
  EXPECT_EQ(probe->outlineCalls, 1u);
  EXPECT_THAT(root.all_of<components::ComputedTextGeometryComponent>(), testing::IsFalse());
  EXPECT_THAT(engine.ensureComputedTextGeometryComponent(root).glyphs, IsEmpty());
}

TEST(TextEngineScriptedTest, ReferenceGeometryMatchesCachedCurvedStretchPlacement) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  const EntityHandle root = MakeStretchedReferenceTextRoot(registry, Visibility::Visible);
  std::size_t work = 100000;

  const auto reference = engine.referenceGeometry(root, true, work);
  const auto& cached = engine.ensureComputedTextGeometryComponent(root);

  ASSERT_THAT(reference, testing::Optional(testing::_));
  ASSERT_THAT(reference->outlines, SizeIs(1));
  ASSERT_THAT(cached.glyphs, SizeIs(1));
  EXPECT_THAT(reference->outlines.front().path.points(),
              testing::ElementsAreArray(cached.glyphs.front().path.points()));
  EXPECT_THAT(reference->outlines.front().path.bounds(),
              BitmapBoundsNear(cached.glyphs.front().extent));
  EXPECT_THAT(reference->outlines.front().path.verbCount(), Gt(5u));
}

TEST(TextEngineScriptedTest, ReferenceGeometryHasAnalyticSharpCornerStretchBounds) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Hidden);
  auto& span = root.get<components::ComputedTextComponent>().spans.front();
  span.xList.clear();
  span.yList.clear();
  span.pathSpline = PathBuilder()
                        .moveTo(Vector2d(0.0, 0.0))
                        .lineTo(Vector2d(5.0, 0.0))
                        .lineTo(Vector2d(5.0, 20.0))
                        .build();
  span.textPathSourceEntity = root.entity();
  span.textPathMethod = components::TextPathMethod::Stretch;
  std::size_t work = 100000;

  const auto geometry = engine.referenceGeometry(root, true, work);

  ASSERT_THAT(geometry, testing::Optional(testing::_));
  ASSERT_THAT(geometry->objectBounds, testing::Optional(testing::_));
  ASSERT_THAT(geometry->outlines, SizeIs(1));
  // Scripted metrics define a full 10x12 cell and a fixed 6x8 outline. The baseline turns at x=5,
  // so incoming +Y and outgoing -X normals give these exact unions.
  EXPECT_THAT(*geometry->objectBounds, BitmapBoundsNear(Box2d::FromXYWH(0.0, -10.0, 15.0, 15.0)));
  EXPECT_THAT(geometry->outlines.front().path.bounds(),
              BitmapBoundsNear(Box2d::FromXYWH(0.0, -8.0, 13.0, 9.0)));
}

TEST(TextEngineScriptedTest, CurvedStretchReferenceBudgetRejectsBeforeOutlineDecode) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeStretchedReferenceTextRoot(registry, Visibility::Hidden);
  std::size_t work = 3000;

  EXPECT_THAT(engine.referenceGeometry(root, false, work), testing::Optional(testing::_));
  work = 3000;
  EXPECT_THAT(engine.referenceGeometry(root, true, work), testing::Eq(std::nullopt));
  EXPECT_THAT(probe->outlineCalls, Eq(0u));
  EXPECT_THAT(work, Eq(0u));
}

TEST(TextEngineScriptedTest, ReferenceInputBudgetRejectsBeforeLayout) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Visible);
  std::size_t work = 0;
  EXPECT_THAT(engine.referenceGeometry(root, false, work), testing::Eq(std::nullopt));
  EXPECT_EQ(engine.layoutCallCountForTesting(), 0u);
  EXPECT_EQ(work, 0u);
}

TEST(TextEngineScriptedTest, ReferenceVerticalCellsRefuseInsteadOfReturningZeroWidthBounds) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Visible);
  root.get<components::ComputedStyleComponent>().properties->writingMode.set(
      WritingMode::VerticalRl, css::Specificity::Override());
  std::size_t work = 100000;
  EXPECT_THAT(engine.referenceGeometry(root, false, work), testing::Eq(std::nullopt));
  EXPECT_EQ(engine.layoutCallCountForTesting(), 0u);
  EXPECT_EQ(work, 0u);
}

TEST(TextEngineScriptedTest, ReferenceOutlineBudgetRejectsBeforeDecodeAndReturnsNoPartialGeometry) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  const ScriptedTextBackend* probe = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const EntityHandle root = MakeReferenceTextRoot(registry, Visibility::Visible);
  std::size_t work = 16;
  EXPECT_THAT(engine.referenceGeometry(root, true, work), testing::Eq(std::nullopt));
  EXPECT_EQ(probe->outlineCalls, 0u);
  EXPECT_EQ(work, 0u);
  EXPECT_THAT(root.all_of<components::ComputedTextGeometryComponent>(), testing::IsFalse());
}

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
  // A, combining acute, and B each consume one DOM UTF-16 unit.
  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 3);

  const std::vector<Path> glyphPaths = engine.computedGlyphPaths(rootHandle);
  EXPECT_THAT(glyphPaths, SizeIs(3));
  EXPECT_THAT(engine.computedInkBounds(rootHandle).width(), Gt(0.0));
  EXPECT_THAT(engine.computedObjectBoundingBox(rootHandle).height(), Gt(0.0));
  EXPECT_THAT(engine.getComputedTextLength(rootHandle), Gt(0.0));
  EXPECT_THAT(engine.getSubStringLength(rootHandle, 0, 1), Gt(0.0));
  EXPECT_EQ(engine.getStartPositionOfChar(rootHandle, 3), Vector2d());
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

TEST(TextEngineScriptedTest, SupplementaryCoordinatesUseCodepointSlot) {
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

TEST(TextEngineScriptedTest, SupplementaryFollowingCodepointCoordinatesAreAppliedOnce) {
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

TEST(TextEngineScriptedTest, AllPositioningAttributesUseUnicodeCodepointIndices) {
  for (const char32_t middle : {U'\u00E9', U'\U0001F600'}) {
    SCOPED_TRACE(static_cast<uint32_t>(middle));
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    std::string value = "A";
    Utf8::Append(middle, std::back_inserter(value));
    value += "B";
    components::ComputedTextComponent text;
    auto span = MakeSpan(value);
    span.xList = {Lengthd(10), Lengthd(20), Lengthd(30)};
    span.yList = {Lengthd(11), Lengthd(21), Lengthd(31)};
    span.dxList = {Lengthd(1), Lengthd(2), Lengthd(3)};
    span.dyList = {Lengthd(7), Lengthd(8), Lengthd(9)};
    span.rotateList = {10, 20, 30};
    text.spans.push_back(std::move(span));
    const auto runs = engine.layout(text, MakeTextParams(20));
    const double middleX = middle >= 0x2E80 ? 24 : 22;
    const double middleY = middle >= 0x2E80 ? 32 : 29;
    EXPECT_THAT(runs,
                ElementsAre(RunGlyphsAre(ElementsAre(
                    AllOf(GlyphXPositionIs(DoubleEq(11)), GlyphYPositionIs(DoubleEq(18)),
                          GlyphRotateDegreesIs(DoubleEq(10))),
                    AllOf(GlyphXPositionIs(DoubleEq(middleX)), GlyphYPositionIs(DoubleEq(middleY)),
                          GlyphRotateDegreesIs(DoubleEq(20))),
                    AllOf(GlyphXPositionIs(DoubleEq(33)), GlyphYPositionIs(DoubleEq(40)),
                          GlyphRotateDegreesIs(DoubleEq(30)))))));
  }
}

/// Sideways glyphs sit on the scripted font's central baseline, the ascent/descent midpoint in em
/// coordinates: (1000 - 200) / 2 * 20 / 1000 = 8px at font-size 20. The em scale is a float.
constexpr double kSidewaysCentralOffset = 8.0;

TEST(TextEngineScriptedTest, VerticalPositioningAttributesUseUnicodeCodepointIndices) {
  for (const char32_t middle : {U'\u00E9', U'\U0001F600'}) {
    SCOPED_TRACE(static_cast<uint32_t>(middle));
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    std::string value = "A";
    Utf8::Append(middle, std::back_inserter(value));
    value += "B";
    components::ComputedTextComponent text;
    auto span = MakeSpan(value);
    span.xList = {Lengthd(10), Lengthd(20), Lengthd(30)};
    span.yList = {Lengthd(11), Lengthd(21), Lengthd(31)};
    span.dxList = {Lengthd(1), Lengthd(2), Lengthd(3)};
    span.dyList = {Lengthd(7), Lengthd(8), Lengthd(9)};
    span.rotateList = {10, 20, 30};
    text.spans.push_back(std::move(span));
    auto params = MakeTextParams(20);
    params.writingMode = WritingMode::VerticalRl;
    const auto runs = engine.layout(text, params);
    const bool upright = middle >= 0x2E80;
    EXPECT_THAT(
        runs,
        ElementsAre(RunGlyphsAre(ElementsAre(
            AllOf(GlyphXPositionIs(DoubleNear(11 - kSidewaysCentralOffset, 1e-5)),
                  GlyphYPositionIs(DoubleEq(18)), GlyphRotateDegreesIs(DoubleEq(100))),
            AllOf(GlyphXPositionIs(DoubleNear(upright ? 24 : 22 - kSidewaysCentralOffset, 1e-5)),
                  GlyphYPositionIs(DoubleEq(upright ? 32 : 29)),
                  GlyphRotateDegreesIs(DoubleEq(upright ? 20 : 110))),
            AllOf(GlyphXPositionIs(DoubleNear(33 - kSidewaysCentralOffset, 1e-5)),
                  GlyphYPositionIs(DoubleEq(40)), GlyphRotateDegreesIs(DoubleEq(120)))))));
  }
}

TEST(TextEngineScriptedTest, CombiningGroupSkipsAbsoluteAndCarriesRelativePositions) {
  for (const bool vertical : {false, true}) {
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager, false, std::nullopt, true);
    components::ComputedTextComponent text;
    auto span = MakeSpan(
        "A\xCC\x81"
        "B");
    span.xList = {Lengthd(10), Lengthd(99), Lengthd(30)};
    span.yList = {Lengthd(11), Lengthd(99), Lengthd(31)};
    span.dxList = {Lengthd(1), Lengthd(2), Lengthd(3)};
    span.dyList = {Lengthd(7), Lengthd(8), Lengthd(9)};
    span.rotateList = {10, 80, 30};
    text.spans.push_back(std::move(span));
    auto params = MakeTextParams(20);
    if (vertical) {
      params.writingMode = WritingMode::VerticalRl;
    }
    const auto runs = engine.layout(text, params);
    ASSERT_EQ(runs.size(), 1u);
    ASSERT_EQ(runs[0].glyphs.size(), 3u);
    const double baselineOffset = vertical ? kSidewaysCentralOffset : 0;
    EXPECT_THAT(
        runs[0].glyphs.front(),
        AllOf(GlyphXPositionIs(DoubleNear(11 - baselineOffset, 1e-5)),
              GlyphYPositionIs(DoubleEq(18)), GlyphRotateDegreesIs(DoubleEq(vertical ? 100 : 10))));
    EXPECT_THAT(runs[0].glyphs[1], GlyphRotateDegreesIs(DoubleEq(vertical ? 100 : 10)));
    EXPECT_THAT(
        runs[0].glyphs.back(),
        AllOf(GlyphXPositionIs(DoubleNear(35 - baselineOffset, 1e-5)),
              GlyphYPositionIs(DoubleEq(48)), GlyphRotateDegreesIs(DoubleEq(vertical ? 120 : 30))));
  }
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
  first.pathStartOffset = 20.0;
  first.textAnchor = TextAnchor::Middle;

  auto continued = MakeSpan("C");
  continued.startsNewChunk = false;
  continued.pathSpline = path;
  continued.textPathSourceEntity = textPathEntity;

  auto hidden = MakeSpan("D");
  hidden.pathSpline = path;
  hidden.textPathSourceEntity = textPathEntity;
  hidden.xList = {Lengthd(80.0, Lengthd::Unit::None)};
  hidden.visibility = Visibility::Hidden;

  text.spans.push_back(std::move(first));
  text.spans.push_back(std::move(continued));
  text.spans.push_back(std::move(hidden));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  const double firstOrigin = 20.0 - (3.0 * 10.0 + 1.0 + 3.0) / 2.0;
  EXPECT_THAT(
      runs,
      ElementsAre(AllOf(RunOnPathIs(Eq(true)),
                        RunGlyphsAre(ElementsAre(
                            GlyphXPositionIs(DoubleNear(firstOrigin, 0.001)),
                            GlyphXPositionIs(DoubleNear(firstOrigin + 10.0 + 1.0, 0.001))))),
                  AllOf(RunOnPathIs(Eq(true)), RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(
                                                   firstOrigin + 2.0 * 10.0 + 1.0 + 3.0, 0.001))))),
                  AllOf(RunOnPathIs(Eq(true)),
                        RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleNear(100.0, 0.001)))))));
}

TEST(TextEngineScriptedTest, StretchTextPathRotatesShapedOffsetsLikeAlign) {
  // On a straight horizontal path, stretch and align place a glyph origin identically. The
  // scripted backend gives this CJK glyph a shaped offset of (2, 3), which an authored rotate must
  // turn before it becomes a path distance and normal offset.
  const auto layoutWith = [](components::TextPathMethod method) {
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    auto span = MakeSpan("\xe4\xb8\xad");
    span.pathSpline = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(200.0, 0.0)).build();
    span.textPathSourceEntity = registry.create();
    span.textPathMethod = method;
    span.rotateList = {90.0};
    text.spans.push_back(std::move(span));
    return engine.layout(text, MakeTextParams(20.0));
  };

  const auto aligned = layoutWith(components::TextPathMethod::Align);
  const auto stretched = layoutWith(components::TextPathMethod::Stretch);

  ASSERT_THAT(aligned, SizeIs(1));
  ASSERT_THAT(stretched, SizeIs(1));
  ASSERT_THAT(aligned.front().glyphs, SizeIs(1));
  ASSERT_THAT(stretched.front().glyphs, SizeIs(1));
  const TextGlyph& alignGlyph = aligned.front().glyphs.front();
  const TextGlyph& stretchGlyph = stretched.front().glyphs.front();
  ASSERT_THAT(stretchGlyph.textPathOrigin, testing::Optional(testing::_));
  EXPECT_THAT(*stretchGlyph.textPathOrigin, DoubleNear(alignGlyph.xPosition, 1e-9));
  EXPECT_THAT(stretchGlyph.textPathNormalOffset, DoubleNear(alignGlyph.yPosition, 1e-9));
}

struct ParsedPathRunDetails {
  std::string text;
  double computedFontSize = 0.0;
  float usedFontSizePx = 0.0f;
  std::vector<TextGlyph> glyphs;
};

std::vector<TextGlyph> LayoutParsedTextPath(std::string_view markup, bool scripted = true,
                                            std::string_view pathData = "M0 0H500",
                                            bool clusterCombiningMarks = false,
                                            std::string_view pathAttributes = {},
                                            std::vector<ParsedPathRunDetails>* details = nullptr,
                                            double rootFontSize = 20.0) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg' width='500' height='200'><path id='p' d='" +
      std::string(pathData) + "' " + std::string(pathAttributes) + "/><text font-size='" +
      std::to_string(rootFontSize) + "'>" + std::string(markup) + "</text></svg>";
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed.hasResult(), Eq(true));
  if (!parsed.hasResult()) {
    return {};
  }
  SVGDocument document = std::move(parsed).result();
  Registry& registry = document.registry();
  FontManager fontManager(registry);
  auto scriptedBackend = std::make_unique<ScriptedTextBackend>();
  scriptedBackend->clusterCombiningMarks = clusterCombiningMarks;
  auto engine =
      scripted ? std::make_unique<TextEngine>(fontManager, registry, std::move(scriptedBackend))
               : std::make_unique<TextEngine>(fontManager, registry);
  const auto textElement = document.querySelector("text");
  const EntityHandle handle = textElement->unsafeEntityHandle();
  engine->prepareForElement(handle, warnings);
  auto& text = handle.get<components::ComputedTextComponent>();
  engine->resolvePerSpanLayoutStyles(handle, text);
  const auto runs = engine->layout(text, MakeTextParams(rootFontSize));
  std::vector<TextGlyph> result;
  for (std::size_t index = 0; index < runs.size(); ++index) {
    const auto& run = runs[index];
    const auto& span = text.spans[index];
    if (details && span.end > span.start) {
      details->push_back({std::string(span.text.data() + span.start, span.end - span.start),
                          span.fontSize.value, run.usedFontSizePx, run.glyphs});
    }
    result.insert(result.end(), run.glyphs.begin(), run.glyphs.end());
  }
  return result;
}

TEST(TextEngineScriptedTest, PostTextPathCursorOpenEndpointIgnoresTextExtent) {
  for (std::string_view label : {"A", "AAAA"}) {
    SCOPED_TRACE(label);
    const std::string markup = "<textPath href='#p'>" + std::string(label) + "</textPath>B";
    const auto glyphs = LayoutParsedTextPath(markup, true, "M0 50H100");
    ASSERT_THAT(glyphs, SizeIs(label.size() + 1));
    EXPECT_THAT(glyphs.back(), AllOf(GlyphXPositionIs(DoubleEq(100.0)),
                                     GlyphYPositionIs(DoubleEq(50.0)), GlyphIndexIs(Not(Eq(0)))));
  }
}

TEST(TextEngineScriptedTest, PostTextPathCursorPolylineEndpoint) {
  const auto glyphs =
      LayoutParsedTextPath("<textPath href='#p'>A</textPath>B", true, "M0 0H100V40");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[0], AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(0.0)),
                               GlyphIndexIs(Not(Eq(0)))));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(40.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorExplicitXYOverridesEndpoint) {
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'>A</textPath><tspan x='40' y='70'>B</tspan>", true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphYPositionIs(DoubleEq(70.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorXOnlyUsesOrdinaryUnspecifiedAxis) {
  const auto ordinary = LayoutParsedTextPath("A<tspan x='40'>B</tspan>");
  ASSERT_THAT(ordinary, SizeIs(2));
  EXPECT_THAT(ordinary[1],
              AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphYPositionIs(DoubleEq(0.0))));
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'>A</textPath><tspan x='40'>B</tspan>", true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphYPositionIs(DoubleEq(0.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorYOnlyUsesOrdinaryUnspecifiedAxis) {
  const auto ordinary = LayoutParsedTextPath("A<tspan y='70'>B</tspan>");
  ASSERT_THAT(ordinary, SizeIs(2));
  EXPECT_THAT(ordinary[1],
              AllOf(GlyphXPositionIs(DoubleEq(10.0)), GlyphYPositionIs(DoubleEq(70.0))));
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'>A</textPath><tspan y='70'>B</tspan>", true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(10.0)), GlyphYPositionIs(DoubleEq(70.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorFirstRelativeShiftIsAbsorbedByEndpoint) {
  // B/C share one shaping run: advance 10, within-run kern 1, then C's relative x shift 2.
  constexpr std::string_view tail = "<tspan dx='7 2' dy='9 3'>BC</tspan>";
  const auto ordinary = LayoutParsedTextPath(tail);
  EXPECT_THAT(
      ordinary,
      ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(7.0)), GlyphYPositionIs(DoubleEq(9.0))),
                  AllOf(GlyphXPositionIs(DoubleEq(20.0)), GlyphYPositionIs(DoubleEq(12.0)))));
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p'>A</textPath>" + std::string(tail),
                                           true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(3));
  EXPECT_THAT(glyphs[1],
              AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(50.0))));
  EXPECT_THAT(glyphs[2],
              AllOf(GlyphXPositionIs(DoubleEq(113.0)), GlyphYPositionIs(DoubleEq(53.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorConsecutiveOwnersResetPathState) {
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p' startOffset='17'><tspan dy='5'>A</tspan></textPath>"
      "<textPath path='M200 80H300'>B</textPath>",
      true, "M0 50H100");
  EXPECT_THAT(
      glyphs,
      ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(17.0)), GlyphYPositionIs(DoubleEq(55.0))),
                  AllOf(GlyphXPositionIs(DoubleEq(200.0)), GlyphYPositionIs(DoubleEq(80.0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorClippedTailStillUsesEndpoint) {
  const auto glyphs =
      LayoutParsedTextPath("<textPath href='#p'>ABCD</textPath>E", true, "M20 50H50");
  ASSERT_THAT(glyphs, SizeIs(5));
  EXPECT_THAT(glyphs[2], GlyphIndexIs(Not(Eq(0))));
  EXPECT_THAT(glyphs[3], GlyphIndexIs(Eq(0)));
  EXPECT_THAT(glyphs[4], AllOf(GlyphXPositionIs(DoubleEq(50.0)), GlyphYPositionIs(DoubleEq(50.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorAllOffPathStillUsesEndpoint) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p' startOffset='200'>A</textPath>B",
                                           true, "M0 50H100");
  EXPECT_THAT(glyphs, ElementsAre(GlyphIndexIs(Eq(0)), AllOf(GlyphXPositionIs(DoubleEq(100.0)),
                                                             GlyphYPositionIs(DoubleEq(50.0)),
                                                             GlyphIndexIs(Not(Eq(0))))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorEmptyTagDoesNotEstablishPathState) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p'/>B", true, "M0 50H100");
  EXPECT_THAT(glyphs,
              ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(0.0)),
                                GlyphIndexIs(Not(Eq(0))))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorEmptyDifferentOwnerDoesNotInventEndpoint) {
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'>A</textPath><textPath path='M200 80H300'/>B", true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(50.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorNonemptyHiddenTextStillEstablishesScope) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p' visibility='hidden'>A</textPath>B",
                                           true, "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(50.0)),
                               GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorDisplayNoneDoesNotEstablishScope) {
  const auto glyphs =
      LayoutParsedTextPath("<textPath href='#p' display='none'>A</textPath>B", true, "M0 50H100");
  EXPECT_THAT(glyphs,
              ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(0.0)),
                                GlyphIndexIs(Not(Eq(0))))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorNonemptyZeroSizeTextStillEstablishesScope) {
  std::vector<ParsedPathRunDetails> parent;
  LayoutParsedTextPath("<textPath href='#p'>A</textPath>B", true, "M0 50H100", false, {}, &parent);
  ASSERT_THAT(parent, SizeIs(2));
  ASSERT_THAT(parent[0].usedFontSizePx, FloatEq(20.0f));
  ASSERT_THAT(parent[0].glyphs, ElementsAre(GlyphXAdvanceIs(DoubleEq(10.0))));
  std::vector<ParsedPathRunDetails> details;
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p' font-size='0'>A</textPath>B", true,
                                           "M0 50H100", false, {}, &details);
  ASSERT_THAT(details, SizeIs(2));
  ASSERT_THAT(details[0].text, Eq("A"));
  ASSERT_THAT(details[0].computedFontSize, DoubleEq(0.0));
  // A failed size/addressability premise is a producer defect, not a zero-size cursor proof.
  ASSERT_THAT(details[0].usedFontSizePx, FloatEq(0.0f));
  ASSERT_THAT(details[0].glyphs,
              ElementsAre(AllOf(GlyphIndexIs(Eq(0)), GlyphXAdvanceIs(DoubleEq(0.0)))));
  ASSERT_THAT(glyphs, Not(IsEmpty()));
  EXPECT_THAT(glyphs.back(), AllOf(GlyphXPositionIs(DoubleEq(100.0)),
                                   GlyphYPositionIs(DoubleEq(50.0)), GlyphIndexIs(Not(Eq(0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorChildZeroProducerDoesNotInheritPositiveSize) {
  std::vector<ParsedPathRunDetails> details;
  LayoutParsedTextPath("<textPath href='#p' font-size='0'>A</textPath>B", true, "M0 50H100", false,
                       {}, &details);
  ASSERT_THAT(details, SizeIs(2));
  ASSERT_THAT(details[0].text, Eq("A"));
  ASSERT_THAT(details[0].computedFontSize, DoubleEq(0.0));
  ASSERT_THAT(details[1].usedFontSizePx, FloatEq(20.0f));
  EXPECT_THAT(details[0].usedFontSizePx, FloatEq(0.0f));
  EXPECT_THAT(details[0].glyphs,
              ElementsAre(AllOf(GlyphIndexIs(Eq(0)), GlyphXAdvanceIs(DoubleEq(0.0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorRootZeroIsolationValidatesAddressabilityFirst) {
  std::vector<ParsedPathRunDetails> details;
  const auto glyphs =
      LayoutParsedTextPath("<textPath href='#p'>A</textPath><tspan font-size='20'>B</tspan>", true,
                           "M0 50H100", false, {}, &details, 0.0);
  ASSERT_THAT(details, SizeIs(2));
  ASSERT_THAT(details[0].text, Eq("A"));
  ASSERT_THAT(details[0].computedFontSize, DoubleEq(0.0));
  ASSERT_THAT(details[0].usedFontSizePx, FloatEq(0.0f));
  ASSERT_THAT(details[1].usedFontSizePx, FloatEq(20.0f));
  // Unlike child-zero inheritance, an absent zero-size character is an addressability defect.
  ASSERT_THAT(details[0].glyphs,
              ElementsAre(AllOf(GlyphIndexIs(Eq(0)), GlyphXAdvanceIs(DoubleEq(0.0)))));
  ASSERT_THAT(glyphs, Not(IsEmpty()));
  EXPECT_THAT(glyphs.back(),
              AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(50.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorInvalidReferencePreservesOrdinaryCompatibility) {
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#missing'>A</textPath><tspan x='40' y='70'>B</tspan>", true, "M0 50H100");
  EXPECT_THAT(glyphs, ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(40.0)),
                                        GlyphYPositionIs(DoubleEq(70.0)))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorReferencedTransformUsesEffectiveEndpoint) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p'>A</textPath>B", true, "M0 50H100",
                                           false, "transform='matrix(0 2 -1 0 70 5)'");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[0], AllOf(GlyphXPositionIs(DoubleNear(20.0, 1e-9)),
                               GlyphYPositionIs(DoubleNear(5.0, 1e-9)),
                               GlyphRotateDegreesIs(DoubleNear(90.0, 1e-9))));
  EXPECT_THAT(glyphs[1],
              AllOf(GlyphXPositionIs(DoubleEq(20.0)), GlyphYPositionIs(DoubleEq(205.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorReversedPathUsesEffectiveEndpoint) {
  const auto glyphs =
      LayoutParsedTextPath("<textPath href='#p' side='right'>A</textPath>B", true, "M40 100H180");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[1],
              AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphYPositionIs(DoubleEq(100.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorCompoundOpenEndIsNotSingleClosedLoop) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p' startOffset='5'>A</textPath>B",
                                           true, "M0 0H10Z M20 30H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[0], GlyphIndexIs(Not(Eq(0))));
  EXPECT_THAT(glyphs[1],
              AllOf(GlyphXPositionIs(DoubleEq(100.0)), GlyphYPositionIs(DoubleEq(30.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorTwoClosedContoursUseFinalEndpoint) {
  const auto glyphs = LayoutParsedTextPath("<textPath href='#p' startOffset='5'>A</textPath>B",
                                           true, "M0 0H10V10H0Z M20 30H30V40H20Z");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[0], GlyphIndexIs(Not(Eq(0))));
  EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(20.0)), GlyphYPositionIs(DoubleEq(30.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorClosedLoopAdjustsStartOffset) {
  for (const auto& [offset, endpoint] :
       std::array<std::pair<int, Vector2d>, 2>{{{5, {5.0, 0.0}}, {-5, {0.0, 5.0}}}}) {
    SCOPED_TRACE(offset);
    const auto glyphs = LayoutParsedTextPath(
        "<textPath href='#p' startOffset='" + std::to_string(offset) + "'>A</textPath>B", true,
        "M0 0H10V10H0Z");
    ASSERT_THAT(glyphs, SizeIs(2));
    EXPECT_THAT(glyphs[0], GlyphIndexIs(Not(Eq(0))));
    EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(endpoint.x)),
                                 GlyphYPositionIs(DoubleEq(endpoint.y))));
  }
}

TEST(TextEngineScriptedTest, PostTextPathCursorClosedMultiCircuitAlsoRequiresVisibleGlyphWrapping) {
  for (int offset : {42, -38}) {
    SCOPED_TRACE(offset);
    const auto glyphs = LayoutParsedTextPath(
        "<textPath href='#p' startOffset='" + std::to_string(offset) + "'>A</textPath>B", true,
        "M0 0H10V10H0Z");
    ASSERT_THAT(glyphs, SizeIs(2));
    EXPECT_THAT(glyphs[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleNear(2.0, 1e-9)),
                                 GlyphYPositionIs(DoubleNear(0.0, 1e-9)),
                                 GlyphRotateDegreesIs(DoubleNear(0.0, 1e-9))));
    EXPECT_THAT(glyphs[1], AllOf(GlyphXPositionIs(DoubleEq(2.0)), GlyphYPositionIs(DoubleEq(0.0))));
  }
}

TEST(TextEngineScriptedTest, PostTextPathCursorWrappedCoordinatesAndOrientationAvoidCorners) {
  for (int offset : {385, -415}) {
    SCOPED_TRACE(offset);
    const auto glyphs = LayoutParsedTextPath(
        "<textPath href='#p' startOffset='" + std::to_string(offset) + "'>A</textPath>B", true,
        "M0 0H100V100H0Z");
    ASSERT_THAT(glyphs, SizeIs(2));
    EXPECT_THAT(glyphs[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleNear(0.0, 1e-9)),
                                 GlyphYPositionIs(DoubleNear(15.0, 1e-9)),
                                 GlyphRotateDegreesIs(DoubleNear(-90.0, 1e-9))));
    EXPECT_THAT(glyphs[1],
                AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(15.0))));
  }
}

TEST(TextEngineScriptedTest, PostTextPathCursorClosedChunkUsesItsActualAnchorAndReference) {
  const auto reference =
      LayoutParsedTextPath("<textPath href='#p'>A<tspan text-anchor='start'>B</tspan></textPath>",
                           true, "M0 0H100V100H0Z");
  ASSERT_THAT(reference, SizeIs(2));
  ASSERT_THAT(reference[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(0.0)),
                                  GlyphYPositionIs(DoubleEq(0.0))));
  ASSERT_THAT(reference[1], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(13.0)),
                                  GlyphYPositionIs(DoubleEq(0.0))));

  // The split backend advances 10 with a boundary kern of 3. Its second midpoint is 18.
  const auto sameChunk =
      LayoutParsedTextPath("<textPath href='#p'>A<tspan text-anchor='end'>B</tspan></textPath>",
                           true, "M0 0H100V100H0Z");
  ASSERT_THAT(sameChunk, SizeIs(2));
  EXPECT_THAT(sameChunk[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(0.0)),
                                  GlyphYPositionIs(DoubleEq(0.0))));
  EXPECT_THAT(sameChunk[1],
              AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(13.0)),
                    GlyphYPositionIs(DoubleEq(0.0)), GlyphRotateDegreesIs(DoubleNear(0.0, 1e-9))));

  // Absolute x40 and end anchor give midpoint35; it lies outside the end window[-400,0].
  const auto newChunk = LayoutParsedTextPath(
      "<textPath href='#p'>A<tspan x='40' text-anchor='end'>B</tspan></textPath>", true,
      "M0 0H100V100H0Z");
  ASSERT_THAT(newChunk, SizeIs(2));
  EXPECT_THAT(newChunk[1], GlyphIndexIs(Eq(0)));
}

void CheckClosedAbsoluteXPathCase(double absoluteX, bool visible) {
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg' width='500' height='200'>"
      "<path id='p' d='M20 30H45V55H20Z'/><text font-size='20'>"
      "<textPath id='owner' href='#p'><tspan id='first'>A</tspan><tspan id='offset' x='" +
      std::to_string(absoluteX) +
      "'>B</tspan></textPath><tspan id='tail' x='40' y='70'>C</tspan>"
      "</text></svg>";
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_TRUE(parsed.hasResult());
  SVGDocument document = std::move(parsed).result();
  Registry& registry = document.registry();
  FontManager manager(registry);
  TextEngine engine(manager, registry, std::make_unique<ScriptedTextBackend>());
  const auto root = document.querySelector("text");
  const auto owner = document.querySelector("#owner");
  const auto first = document.querySelector("#first");
  const auto offset = document.querySelector("#offset");
  const auto tail = document.querySelector("#tail");
  ASSERT_TRUE(root.has_value());
  ASSERT_TRUE(owner.has_value());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(offset.has_value());
  ASSERT_TRUE(tail.has_value());
  const auto handle = root->unsafeEntityHandle();
  const auto ownerEntity = owner->unsafeEntityHandle().entity();
  engine.prepareForElement(handle, warnings);
  auto& text = handle.get<components::ComputedTextComponent>();
  const auto firstSpan = std::find_if(text.spans.begin(), text.spans.end(), [&](const auto& span) {
    return span.sourceEntity == first->unsafeEntityHandle().entity();
  });
  const auto offsetSpan = std::find_if(text.spans.begin(), text.spans.end(), [&](const auto& span) {
    return span.sourceEntity == offset->unsafeEntityHandle().entity();
  });
  const auto tailSpan = std::find_if(text.spans.begin(), text.spans.end(), [&](const auto& span) {
    return span.sourceEntity == tail->unsafeEntityHandle().entity();
  });
  ASSERT_NE(firstSpan, text.spans.end());
  ASSERT_NE(offsetSpan, text.spans.end());
  ASSERT_NE(tailSpan, text.spans.end());
  ASSERT_EQ(firstSpan->textPathSourceEntity, ownerEntity);
  ASSERT_EQ(offsetSpan->textPathSourceEntity, ownerEntity);
  ASSERT_THAT(offsetSpan->xList, SizeIs(1));
  ASSERT_TRUE(offsetSpan->xList[0].has_value());
  ASSERT_THAT(offsetSpan->xList[0]->value, DoubleEq(absoluteX));
  ASSERT_TRUE(std::isfinite(offsetSpan->xList[0]->toPixels(
      MakeTextParams(20).viewBox, MakeTextParams(20).fontMetrics, Lengthd::Extent::X)));
  // Save integer indices before cache preparation can mutate the span container.
  const size_t firstRun = static_cast<size_t>(firstSpan - text.spans.begin());
  const size_t offsetRun = static_cast<size_t>(offsetSpan - text.spans.begin());
  const size_t tailRun = static_cast<size_t>(tailSpan - text.spans.begin());
  engine.resolvePerSpanLayoutStyles(handle, text);
  const auto& cache = engine.ensureComputedTextGeometryComponent(handle);
  ASSERT_LT(firstRun, cache.runs.size());
  ASSERT_LT(offsetRun, cache.runs.size());
  ASSERT_LT(tailRun, cache.runs.size());
  ASSERT_THAT(cache.runs[firstRun].glyphs, SizeIs(1));
  ASSERT_THAT(cache.runs[offsetRun].glyphs, SizeIs(1));
  ASSERT_THAT(cache.runs[tailRun].glyphs, SizeIs(1));
  ASSERT_THAT(cache.runs[offsetRun].usedFontSizePx, FloatEq(20.0f));
  ASSERT_THAT(cache.runs[offsetRun].glyphs[0].xAdvance, DoubleEq(10.0));
  ASSERT_THAT(cache.runs[firstRun].glyphs[0],
              AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(20.0)),
                    GlyphYPositionIs(DoubleEq(30.0)), GlyphRotateDegreesIs(DoubleEq(0.0))));
  ASSERT_THAT(cache.runs[tailRun].glyphs[0],
              AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(40.0)),
                    GlyphYPositionIs(DoubleEq(70.0))));
  if (visible) {
    EXPECT_THAT(cache.runs[offsetRun].glyphs[0],
                AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(30.0)),
                      GlyphYPositionIs(DoubleEq(30.0)), GlyphRotateDegreesIs(DoubleEq(0.0))));
  } else {
    EXPECT_THAT(cache.runs[offsetRun].glyphs[0], GlyphIndexIs(Eq(0)));
  }
}

TEST(TextEngineScriptedTest, ClosedTextPathAbsoluteXRetainsSingleCircuitClipping) {
  // L100: B midpoints1005/-995 are outside [0,100]; x10 gives admitted midpoint15.
  for (double absoluteX : {1000.0, -1000.0}) {
    SCOPED_TRACE(absoluteX);
    ASSERT_NO_FATAL_FAILURE(CheckClosedAbsoluteXPathCase(absoluteX, false));
  }
  ASSERT_NO_FATAL_FAILURE(CheckClosedAbsoluteXPathCase(10.0, true));
}

TEST(TextEngineScriptedTest, PostTextPathCursorInitialDxEntersNominalPenOnce) {
  const auto ordinary = LayoutParsedTextPath("<tspan dx='5'>A</tspan><tspan y='70'>B</tspan>");
  ASSERT_THAT(ordinary, SizeIs(2));
  ASSERT_THAT(ordinary[0], GlyphXPositionIs(DoubleEq(5.0)));
  ASSERT_THAT(ordinary[1],
              AllOf(GlyphXPositionIs(DoubleEq(15.0)), GlyphYPositionIs(DoubleEq(70.0))));
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'><tspan dx='5'>A</tspan></textPath><tspan y='70'>B</tspan>", true,
      "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  ASSERT_THAT(glyphs[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(5.0)),
                               GlyphYPositionIs(DoubleEq(50.0))));
  EXPECT_THAT(glyphs[1], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(15.0)),
                               GlyphYPositionIs(DoubleEq(70.0))));
}

TEST(TextEngineScriptedTest, PostTextPathCursorInitialDyEntersNominalPenOnce) {
  const auto ordinary = LayoutParsedTextPath("<tspan dy='5'>A</tspan><tspan x='40'>B</tspan>");
  ASSERT_THAT(ordinary, SizeIs(2));
  ASSERT_THAT(ordinary[0], GlyphYPositionIs(DoubleEq(5.0)));
  ASSERT_THAT(ordinary[1],
              AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphYPositionIs(DoubleEq(5.0))));
  const auto glyphs = LayoutParsedTextPath(
      "<textPath href='#p'><tspan dy='5'>A</tspan></textPath><tspan x='40'>B</tspan>", true,
      "M0 50H100");
  ASSERT_THAT(glyphs, SizeIs(2));
  ASSERT_THAT(glyphs[0], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(0.0)),
                               GlyphYPositionIs(DoubleEq(55.0))));
  EXPECT_THAT(glyphs[1], AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(40.0)),
                               GlyphYPositionIs(DoubleEq(5.0))));
}

void CheckFiniteOverflowAuthoredDx(const components::ComputedTextComponent::TextSpan& span,
                                   double displacement) {
  // Check supported tspan-authored input before any layout consumes positioning entries.
  const auto authoredDx = span.dxList;
  ASSERT_THAT(authoredDx, SizeIs(2));
  for (const auto& value : authoredDx) {
    ASSERT_TRUE(value.has_value());
    ASSERT_THAT(value->value, DoubleEq(displacement));
    ASSERT_TRUE(std::isfinite(value->toPixels(MakeTextParams(20).viewBox,
                                              MakeTextParams(20).fontMetrics, Lengthd::Extent::X)));
  }
}

void FindFiniteOverflowRunIndices(components::ComputedTextComponent& text, Entity pathEntity,
                                  Entity pathContentEntity, Entity tailEntity, double displacement,
                                  size_t& pathRun, size_t& tailRun) {
  const auto pathSpan = std::find_if(text.spans.begin(), text.spans.end(), [&](const auto& span) {
    return span.textPathSourceEntity == pathEntity && span.sourceEntity == pathContentEntity &&
           span.end - span.start == 2;
  });
  const auto tailSpan = std::find_if(text.spans.begin(), text.spans.end(), [&](const auto& span) {
    return span.sourceEntity == tailEntity && span.end - span.start == 1;
  });
  ASSERT_NE(pathSpan, text.spans.end());
  ASSERT_NE(tailSpan, text.spans.end());
  ASSERT_NO_FATAL_FAILURE(CheckFiniteOverflowAuthoredDx(*pathSpan, displacement));
  pathRun = static_cast<size_t>(pathSpan - text.spans.begin());
  tailRun = static_cast<size_t>(tailSpan - text.spans.begin());
}

void CheckFiniteOverflowSuppressedPath(const components::ComputedTextGeometryComponent& cache,
                                       size_t pathRun, size_t tailRun, bool explicitTail) {
  ASSERT_LT(pathRun, cache.runs.size());
  ASSERT_LT(tailRun, cache.runs.size());
  ASSERT_THAT(cache.runs[pathRun].usedFontSizePx, FloatEq(20.0f));
  ASSERT_THAT(cache.runs[tailRun].usedFontSizePx, FloatEq(20.0f));
  ASSERT_THAT(cache.runs[pathRun].glyphs, SizeIs(2));
  int suppressedNonfinitePathPositions = 0;
  for (const auto& glyph : cache.runs[pathRun].glyphs) {
    ASSERT_EQ(glyph.glyphIndex, 0);
    suppressedNonfinitePathPositions +=
        !std::isfinite(glyph.xPosition) || !std::isfinite(glyph.yPosition);
  }
  // Suppressed path NaNs are an existing, separate debt, not this tail's causal oracle.
  ::testing::Test::RecordProperty(explicitTail ? "explicit_tail_suppressed_path_nonfinite"
                                               : "implicit_tail_suppressed_path_nonfinite",
                                  suppressedNonfinitePathPositions);
}

void CheckFiniteOverflowTailPositions(const components::ComputedTextGeometryComponent& cache,
                                      EntityHandle tailHandle, size_t tailRun) {
  for (const auto& glyph : cache.runs[tailRun].glyphs) {
    if (glyph.glyphIndex != 0) {
      EXPECT_TRUE(std::isfinite(glyph.xPosition));
      EXPECT_TRUE(std::isfinite(glyph.yPosition));
      EXPECT_TRUE(std::isfinite(glyph.rotateDegrees));
      EXPECT_TRUE(std::isfinite(glyph.xAdvance));
      EXPECT_TRUE(std::isfinite(glyph.yAdvance));
    }
  }
  for (const auto& character : cache.characters) {
    if (character.sourceEntity == tailHandle.entity() && character.rendered) {
      EXPECT_TRUE(std::isfinite(character.startPosition.x));
      EXPECT_TRUE(std::isfinite(character.startPosition.y));
      EXPECT_TRUE(std::isfinite(character.endPosition.x));
      EXPECT_TRUE(std::isfinite(character.endPosition.y));
      EXPECT_TRUE(std::isfinite(character.advance));
      EXPECT_TRUE(std::isfinite(character.rotation));
    }
  }
}

void CheckFiniteOverflowTailInkBounds(const components::ComputedTextGeometryComponent& cache,
                                      EntityHandle tailHandle) {
  for (const auto& glyph : cache.glyphs) {
    if (glyph.sourceEntity == tailHandle.entity()) {
      EXPECT_TRUE(std::isfinite(glyph.extent.topLeft.x));
      EXPECT_TRUE(std::isfinite(glyph.extent.topLeft.y));
      EXPECT_TRUE(std::isfinite(glyph.extent.bottomRight.x));
      EXPECT_TRUE(std::isfinite(glyph.extent.bottomRight.y));
      for (const auto& point : glyph.path.points()) {
        EXPECT_TRUE(std::isfinite(point.x));
        EXPECT_TRUE(std::isfinite(point.y));
      }
    }
  }
  const auto tailBounds = cache.objectBoundsByEntity.find(tailHandle.entity());
  if (tailBounds != cache.objectBoundsByEntity.end()) {
    EXPECT_TRUE(std::isfinite(tailBounds->second.topLeft.x));
    EXPECT_TRUE(std::isfinite(tailBounds->second.topLeft.y));
    EXPECT_TRUE(std::isfinite(tailBounds->second.bottomRight.x));
    EXPECT_TRUE(std::isfinite(tailBounds->second.bottomRight.y));
  }
}

void CheckFiniteOverflowExplicitTail(TextEngine& engine,
                                     const components::ComputedTextGeometryComponent& cache,
                                     EntityHandle tailHandle, size_t tailRun) {
  ASSERT_THAT(cache.runs[tailRun].glyphs, SizeIs(1));
  EXPECT_THAT(cache.runs[tailRun].glyphs.front(),
              AllOf(GlyphIndexIs(Not(Eq(0))), GlyphXPositionIs(DoubleEq(40.0)),
                    GlyphYPositionIs(DoubleEq(70.0)), GlyphXAdvanceIs(DoubleEq(10.0))));
  EXPECT_EQ(engine.getNumberOfChars(tailHandle), 1);
  EXPECT_EQ(engine.getStartPositionOfChar(tailHandle, 0), Vector2d(40.0, 70.0));
  EXPECT_THAT(engine.getSubStringLength(tailHandle, 0, 1), Gt(0.0));
  const auto tailInk =
      std::find_if(cache.glyphs.begin(), cache.glyphs.end(),
                   [&](const auto& glyph) { return glyph.sourceEntity == tailHandle.entity(); });
  ASSERT_NE(tailInk, cache.glyphs.end());
  ASSERT_EQ(
      std::count_if(cache.glyphs.begin(), cache.glyphs.end(),
                    [&](const auto& glyph) { return glyph.sourceEntity == tailHandle.entity(); }),
      1);
  EXPECT_FALSE(tailInk->path.empty());
  EXPECT_EQ(tailInk->extent, Box2d::FromXYWH(40.0, 62.0, 6.0, 8.0));
  EXPECT_EQ(engine.getCharNumAtPosition(tailHandle, Vector2d(43.0, 66.0)), 0);
}

void CheckFiniteOverflowScene(bool explicitTail, double displacement) {
  const std::string tail =
      explicitTail ? "<tspan id='tail' x='40' y='70'>C</tspan>" : "<tspan id='tail'>C</tspan>";
  const std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg' width='500' height='200'>"
      "<path id='p' d='M0 50H100'/><text font-size='20'>"
      "<textPath href='#p'><tspan id='path-content' dx='1e308 1e308'>AB</tspan></textPath>" +
      tail + "</text></svg>";
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_TRUE(parsed.hasResult());
  SVGDocument document = std::move(parsed).result();
  Registry& registry = document.registry();
  FontManager manager(registry);
  TextEngine engine(manager, registry, std::make_unique<ScriptedTextBackend>());
  const auto root = document.querySelector("text");
  const auto pathElement = document.querySelector("textPath");
  const auto pathContent = document.querySelector("#path-content");
  const auto tailElement = document.querySelector("#tail");
  ASSERT_TRUE(root.has_value());
  ASSERT_TRUE(pathElement.has_value());
  ASSERT_TRUE(pathElement->isa<SVGTextPathElement>());
  ASSERT_TRUE(pathContent.has_value());
  ASSERT_TRUE(tailElement.has_value());
  const EntityHandle rootHandle = root->unsafeEntityHandle();
  const EntityHandle tailHandle = tailElement->unsafeEntityHandle();
  engine.prepareForElement(rootHandle, warnings);
  auto& text = rootHandle.get<components::ComputedTextComponent>();
  size_t pathRun = 0;
  size_t tailRun = 0;
  ASSERT_NO_FATAL_FAILURE(FindFiniteOverflowRunIndices(
      text, pathElement->unsafeEntityHandle().entity(), pathContent->unsafeEntityHandle().entity(),
      tailHandle.entity(), displacement, pathRun, tailRun));
  engine.resolvePerSpanLayoutStyles(rootHandle, text);
  const auto& cache = engine.ensureComputedTextGeometryComponent(rootHandle);
  ASSERT_NO_FATAL_FAILURE(CheckFiniteOverflowSuppressedPath(cache, pathRun, tailRun, explicitTail));
  CheckFiniteOverflowTailPositions(cache, tailHandle, tailRun);
  CheckFiniteOverflowTailInkBounds(cache, tailHandle);
  if (explicitTail) {
    ASSERT_NO_FATAL_FAILURE(CheckFiniteOverflowExplicitTail(engine, cache, tailHandle, tailRun));
  }
}

TEST(TextEngineScriptedTest, PostTextPathCursorFiniteDxOverflowQuarantinesOnlyUnrepresentableTail) {
  const double displacement = 1e308;
  ASSERT_TRUE(std::isfinite(displacement));
  ASSERT_FALSE(std::isfinite(displacement + displacement));
  for (bool explicitTail : {false, true}) {
    SCOPED_TRACE(explicitTail);
    ASSERT_NO_FATAL_FAILURE(CheckFiniteOverflowScene(explicitTail, displacement));
  }
}

TEST(TextEngineScriptedTest, TextPathLengthIncludesMixedDirectAndChildContent) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="90" lengthAdjust="spacingAndGlyphs">A<tspan>B</tspan>C</textPath>)");
  ASSERT_THAT(glyphs, SizeIs(3));
  EXPECT_THAT(glyphs.back().xPosition + glyphs.back().xAdvance - glyphs.front().xPosition,
              DoubleNear(90.0, 1e-6));
  EXPECT_THAT(glyphs,
              testing::Each(GlyphStretchScaleXIs(FloatEq(90.0f / (3.0f * 10.0f + 2.0f * 3.0f)))));
}

TEST(TextEngineScriptedTest, TextPathLengthIncludesAllChildContent) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="42" lengthAdjust="spacingAndGlyphs"><tspan>AB</tspan></textPath>)");
  EXPECT_THAT(
      glyphs,
      ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(20.0)),
                        GlyphStretchScaleXIs(FloatEq(2.0f))),
                  AllOf(GlyphXPositionIs(DoubleNear(22.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(20.0)),
                        GlyphStretchScaleXIs(FloatEq(2.0f)))));
}

TEST(TextEngineScriptedTest, TextPathLengthSpacingCountsChildBoundaries) {
  const auto glyphs =
      LayoutParsedTextPath(R"(<textPath href="#p" textLength="90">A<tspan>B</tspan>C</textPath>)");
  EXPECT_THAT(
      glyphs,
      ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(10.0))),
          AllOf(GlyphXPositionIs(DoubleNear(40.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(10.0))),
          AllOf(GlyphXPositionIs(DoubleNear(80.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(10.0)))));
}

TEST(TextEngineScriptedTest, TextPathLengthPreservesNestedOverrides) {
  for (const std::string adjust : {"spacing", "spacingAndGlyphs"}) {
    SCOPED_TRACE(adjust);
    const auto glyphs = LayoutParsedTextPath(
        "<textPath href=\"#p\" textLength=\"100\" lengthAdjust=\"" + adjust +
        R"(">A<tspan textLength="40" lengthAdjust="spacingAndGlyphs">B<tspan>C</tspan>D</tspan>E</textPath>)");
    ASSERT_THAT(glyphs, SizeIs(5));
    EXPECT_THAT(glyphs[3].xPosition + glyphs[3].xAdvance - glyphs[1].xPosition,
                DoubleNear(40.0, 1e-6));
    EXPECT_THAT(glyphs[4].xPosition + glyphs[4].xAdvance - glyphs[0].xPosition,
                DoubleNear(100.0, 1e-6));
    for (size_t i = 1; i <= 3; ++i) {
      EXPECT_THAT(glyphs[i].stretchScaleX, FloatEq(40.0f / (3.0f * 10.0f + 2.0f * 3.0f)));
    }
    EXPECT_THAT(
        glyphs[0].stretchScaleX,
        FloatEq(adjust == "spacing" ? 1.0f : (100.0f - 40.0f) / (2.0f * 10.0f + 2.0f * 3.0f)));
    EXPECT_THAT(glyphs[4].stretchScaleX, FloatEq(glyphs[0].stretchScaleX));
  }
}

TEST(TextEngineScriptedTest, TextPathLengthAppliesChildCoordinateResetsAfterAdjustment) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" startOffset="10" textLength="80" lengthAdjust="spacingAndGlyphs">A<tspan x="50 100" y="20 40">BC</tspan>D</textPath>)");
  ASSERT_THAT(glyphs, SizeIs(4));
  EXPECT_THAT(glyphs[0].xPosition, DoubleNear(10.0, 1e-6));
  EXPECT_THAT(glyphs[1].xPosition, DoubleNear(60.0, 1e-6));
  EXPECT_THAT(glyphs[2].xPosition, DoubleNear(110.0, 1e-6));
  EXPECT_THAT(
      glyphs[3].xPosition,
      DoubleNear(glyphs[2].xPosition + glyphs[2].xAdvance + 3.0 * 80.0 / (4.0 * 10.0 + 3.0), 1e-6));
  EXPECT_THAT(glyphs, testing::Each(GlyphYPositionIs(DoubleNear(0.0, 1e-6))));
}

TEST(TextEngineScriptedTest, TextPathLengthAnchorsTheWholeScopeAtStartOffset) {
  for (const std::string anchor : {"middle", "end"}) {
    SCOPED_TRACE(anchor);
    const auto glyphs = LayoutParsedTextPath(
        "<textPath href=\"#p\" startOffset=\"150\" text-anchor=\"" + anchor +
        R"(" textLength="90" lengthAdjust="spacingAndGlyphs">A<tspan>B</tspan>C</textPath>)");
    ASSERT_THAT(glyphs, SizeIs(3));
    const double expectedStart = anchor == "middle" ? 105.0 : 60.0;
    EXPECT_THAT(glyphs.front().xPosition, DoubleNear(expectedStart, 1e-6));
    EXPECT_THAT(glyphs.back().xPosition + glyphs.back().xAdvance,
                DoubleNear(expectedStart + 90.0, 1e-6));
  }
}

TEST(TextEngineScriptedTest, TextPathLengthHandlesEmptySingleZeroNegativeAndLargeValues) {
  EXPECT_THAT(LayoutParsedTextPath(R"(<textPath href="#p" textLength="100"><tspan/></textPath>)"),
              IsEmpty());
  const auto single =
      LayoutParsedTextPath(R"(<textPath href="#p" textLength="100"><tspan>A</tspan></textPath>)");
  EXPECT_THAT(single, ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)),
                                        GlyphXAdvanceIs(DoubleEq(10.0)))));
  const auto zero = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="0" lengthAdjust="spacingAndGlyphs">A<tspan>B</tspan></textPath>)");
  EXPECT_THAT(zero, testing::Each(AllOf(GlyphXAdvanceIs(DoubleEq(0.0)),
                                        GlyphStretchScaleXIs(FloatEq(0.0f)))));
  const auto negative = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="-10" lengthAdjust="spacingAndGlyphs">A<tspan>B</tspan></textPath>)");
  EXPECT_THAT(negative, testing::Each(AllOf(GlyphXAdvanceIs(DoubleEq(10.0)),
                                            GlyphStretchScaleXIs(FloatEq(1.0f)))));
  const auto large = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="1e30" lengthAdjust="spacingAndGlyphs">A<tspan>B</tspan></textPath>)");
  EXPECT_THAT(large, testing::Each(GlyphIndexIs(Eq(0))));
  for (const TextGlyph& glyph : large) {
    EXPECT_THAT(std::isfinite(glyph.xPosition), Eq(true));
    EXPECT_THAT(std::isfinite(glyph.xAdvance), Eq(true));
    EXPECT_THAT(std::isfinite(glyph.stretchScaleX), Eq(true));
  }
}

TEST(TextEngineScriptedTest, TextPathLengthRetainsHiddenAdvancesAndIgnoresDisplayNone) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="90" lengthAdjust="spacingAndGlyphs">A<tspan visibility="hidden">B</tspan><tspan display="none">ignored</tspan>C</textPath>)");
  // `visibility: hidden` keeps the span in layout (it is skipped at paint time instead), while
  // `display: none` removes it entirely and contributes no advance.
  EXPECT_THAT(
      glyphs,
      ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)),
                GlyphXAdvanceIs(DoubleEq(10.0 * 90.0 / (3.0 * 10.0 + 2.0 * 3.0)))),
          AllOf(GlyphXPositionIs(DoubleNear((10.0 + 3.0) * 90.0 / (3.0 * 10.0 + 2.0 * 3.0), 1e-6)),
                GlyphXAdvanceIs(DoubleEq(10.0 * 90.0 / (3.0 * 10.0 + 2.0 * 3.0)))),
          AllOf(GlyphXPositionIs(
                    DoubleNear(2.0 * (10.0 + 3.0) * 90.0 / (3.0 * 10.0 + 2.0 * 3.0), 1e-6)),
                GlyphXAdvanceIs(DoubleEq(10.0 * 90.0 / (3.0 * 10.0 + 2.0 * 3.0))))));
}

TEST(TextEngineScriptedTest, TextPathLengthWholeScopeUsesSelectedBackend) {
  for (const std::string content : {"A<tspan>B</tspan>C", "<tspan>ABC</tspan>"}) {
    SCOPED_TRACE(content);
    const auto glyphs = LayoutParsedTextPath(
        R"(<textPath href="#p" textLength="100" lengthAdjust="spacingAndGlyphs">)" + content +
            "</textPath>",
        /*scripted=*/false);
    ASSERT_THAT(glyphs, SizeIs(3));
    EXPECT_THAT(glyphs.back().xPosition + glyphs.back().xAdvance - glyphs.front().xPosition,
                DoubleNear(100.0, 1e-5));
  }
}

TEST(TextEngineScriptedTest, TextPathLengthKeepsSpacingOnlyChildWidth) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="100" lengthAdjust="spacingAndGlyphs">A<tspan textLength="40">BC</tspan>D</textPath>)");
  ASSERT_THAT(glyphs, SizeIs(4));
  // The child's one gap grows from its 1px kern by its trailing gap, which D inherits as fixed
  // advance alongside the child's 40px. This follows Donner's fixed-child model for nested length
  // owners; Chrome instead scales the child's content inside a spacingAndGlyphs parent.
  const double childTrailingGap = 40.0 - (2.0 * 10.0 + 1.0);
  EXPECT_THAT(
      glyphs[0].xAdvance,
      DoubleNear(10.0 * (100.0 - 40.0 - childTrailingGap) / (2.0 * 10.0 + 2.0 * 3.0), 1e-6));
  EXPECT_THAT(glyphs[1].xAdvance, DoubleEq(10.0));
  EXPECT_THAT(glyphs[2].xAdvance, DoubleEq(10.0));
  EXPECT_THAT(glyphs[2].xPosition + glyphs[2].xAdvance - glyphs[1].xPosition,
              DoubleNear(40.0, 1e-6));
  EXPECT_THAT(glyphs[3].xPosition + glyphs[3].xAdvance, DoubleNear(100.0, 1e-6));
}

TEST(TextEngineScriptedTest, FirstNestedLengthOwnerKeepsThePathOrigin) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p"><tspan textLength="40" lengthAdjust="spacingAndGlyphs">AB</tspan>C</textPath>)");
  ASSERT_THAT(glyphs, SizeIs(3));
  EXPECT_THAT(glyphs[0].xPosition, DoubleNear(0.0, 1e-6));
  EXPECT_THAT(glyphs[1].xPosition + glyphs[1].xAdvance, DoubleNear(40.0, 1e-6));
  EXPECT_THAT(glyphs[2].xPosition, DoubleNear(43.0, 1e-6));
  EXPECT_THAT(glyphs[2].xAdvance, DoubleEq(10.0));
}

TEST(TextEngineScriptedTest, AbsoluteResetOnLengthOwnerStopsPreviousLengthCarry) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<tspan textLength="40" lengthAdjust="spacingAndGlyphs">AB</tspan><tspan x="100" textLength="20" lengthAdjust="spacingAndGlyphs">C</tspan>)");
  ASSERT_THAT(glyphs, SizeIs(3));
  EXPECT_THAT(glyphs[1].xPosition + glyphs[1].xAdvance - glyphs[0].xPosition,
              DoubleNear(40.0, 1e-6));
  EXPECT_THAT(glyphs[2].xPosition, DoubleNear(100.0, 1e-6));
  EXPECT_THAT(glyphs[2].xAdvance, DoubleNear(20.0, 1e-6));
}

TEST(TextEngineScriptedTest, AbsoluteResetWithinLengthOwnerIsNotAppliedTwice) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p"><tspan textLength="40" lengthAdjust="spacingAndGlyphs" x="0 100">AB</tspan></textPath>)");
  ASSERT_THAT(glyphs, SizeIs(2));
  EXPECT_THAT(glyphs[0].xPosition, DoubleNear(0.0, 1e-6));
  EXPECT_THAT(glyphs[1].xPosition, DoubleNear(100.0, 1e-6));
}

TEST(TextEngineScriptedTest, TextPathLengthKeepsThreeLevelsOfOverrides) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="120" lengthAdjust="spacingAndGlyphs">A<tspan textLength="60" lengthAdjust="spacingAndGlyphs">B<tspan textLength="20" lengthAdjust="spacingAndGlyphs">CD</tspan>E</tspan>F</textPath>)");
  ASSERT_THAT(glyphs, SizeIs(6));
  EXPECT_THAT(glyphs[3].xPosition + glyphs[3].xAdvance - glyphs[2].xPosition,
              DoubleNear(20.0, 1e-6));
  EXPECT_THAT(glyphs[4].xPosition + glyphs[4].xAdvance - glyphs[1].xPosition,
              DoubleNear(60.0, 1e-6));
  EXPECT_THAT(glyphs[5].xPosition + glyphs[5].xAdvance, DoubleNear(120.0, 1e-6));
  const auto allChildren = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="120" lengthAdjust="spacingAndGlyphs"><tspan textLength="40" lengthAdjust="spacingAndGlyphs">AB</tspan></textPath>)");
  ASSERT_THAT(allChildren, SizeIs(2));
  EXPECT_THAT(allChildren.back().xPosition + allChildren.back().xAdvance, DoubleNear(40.0, 1e-6));
}

/// Lays out \p value as one vertical span, optionally fitted to \p target with
/// `lengthAdjust="spacingAndGlyphs"`.
std::vector<TextGlyph> LayoutVerticalSpan(const std::string& value, std::optional<double> target) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan(value);
  if (target) {
    span.textLength = Lengthd(*target, Lengthd::Unit::None);
    span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  }
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);
  return runs.empty() ? std::vector<TextGlyph>{} : runs.front().glyphs;
}

// A glyph's outline is stretched before its rotation is applied. A sideways glyph in vertical text
// is turned a quarter turn, so its own x axis is the inline axis; an upright glyph keeps y inline.
TEST(TextEngineScriptedTest, VerticalSpacingAndGlyphsStretchesEachGlyphAlongTheInlineAxis) {
  struct Case {
    const char* name;
    std::string value;
    double rotateDegrees;
    float stretchX;
    float stretchY;
  };
  std::string upright;
  Utf8::Append(U'一', std::back_inserter(upright));
  Utf8::Append(U'二', std::back_inserter(upright));
  const Case cases[] = {
      {"sideways", "AB", 90.0, 2.0f, 1.0f},
      {"upright", upright, 0.0, 1.0f, 2.0f},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.name);
    const auto natural = LayoutVerticalSpan(test.value, std::nullopt);
    ASSERT_THAT(natural, SizeIs(2));
    const double naturalLength = natural[1].yPosition + natural[1].yAdvance - natural[0].yPosition;
    // The span starts at pen position 0, and an upright glyph's offset from its pen stretches
    // with its outline, so every inline coordinate doubles.
    const auto glyphMatches = [&](const TextGlyph& glyph) {
      return AllOf(GlyphYPositionIs(DoubleNear(2.0 * glyph.yPosition, 1e-9)),
                   GlyphYAdvanceIs(DoubleEq(2.0 * glyph.yAdvance)),
                   GlyphRotateDegreesIs(DoubleEq(test.rotateDegrees)),
                   GlyphStretchScaleXIs(FloatEq(test.stretchX)),
                   GlyphStretchScaleYIs(FloatEq(test.stretchY)));
    };

    EXPECT_THAT(LayoutVerticalSpan(test.value, 2.0 * naturalLength),
                ElementsAre(glyphMatches(natural[0]), glyphMatches(natural[1])));
  }
}

// SVG 2's text-length loop accrues the per-gap adjustment after every character of a
// `lengthAdjust="spacing"` owner, its last included, but does not define how following text
// moves. Chrome shifts it by the accrued amount, one gap past the owner's target length, and the
// resvg references agree; Donner follows them. Glyph stretching accrues no trailing gap.
TEST(TextEngineScriptedTest, SpacingLengthOwnerCarriesItsTrailingGapToFollowingText) {
  const auto natural = LayoutParsedTextPath("<tspan>CD</tspan>EF");
  ASSERT_THAT(natural, SizeIs(4));
  const double naturalLength = natural[1].xPosition + natural[1].xAdvance - natural[0].xPosition;
  const double naturalGap = natural[2].xPosition - (natural[1].xPosition + natural[1].xAdvance);
  const double extra = 20.0;

  for (const std::string adjust : {"spacing", "spacingAndGlyphs"}) {
    SCOPED_TRACE(adjust);
    const auto glyphs =
        LayoutParsedTextPath("<tspan textLength=\"" + std::to_string(naturalLength + extra) +
                             "\" lengthAdjust=\"" + adjust + "\">CD</tspan>EF");
    ASSERT_THAT(glyphs, SizeIs(4));
    const double ownerEnd = glyphs[1].xPosition + glyphs[1].xAdvance;
    const double trailingGap = adjust == "spacing" ? extra : 0.0;
    EXPECT_THAT(std::vector<double>({ownerEnd - glyphs[0].xPosition, glyphs[2].xPosition - ownerEnd,
                                     glyphs[3].xPosition - glyphs[2].xPosition}),
                ElementsAre(DoubleNear(naturalLength + extra, 1e-9),
                            DoubleNear(naturalGap + trailingGap, 1e-9),
                            DoubleNear(natural[3].xPosition - natural[2].xPosition, 1e-9)));
  }
}

// A nested spacing owner's trailing gap belongs to the text after it, so an enclosing owner that it
// ends measures and fits its own length without it, and passes the gap on to its own followers.
// This follows Donner's model of nested length owners as fixed-width children.
TEST(TextEngineScriptedTest, NestedSpacingOwnerPassesItsTrailingGapThroughItsLastAncestor) {
  const auto natural = LayoutParsedTextPath("<tspan>A<tspan>CD</tspan></tspan>EF");
  const auto glyphs = LayoutParsedTextPath(
      R"(<tspan textLength="60" lengthAdjust="spacingAndGlyphs">A<tspan textLength="40">CD</tspan></tspan>EF)");
  ASSERT_THAT(natural, SizeIs(5));
  ASSERT_THAT(glyphs, SizeIs(5));
  const double innerGap =
      (glyphs[2].xPosition - glyphs[1].xPosition) - (natural[2].xPosition - natural[1].xPosition);
  const double naturalGap = natural[3].xPosition - (natural[2].xPosition + natural[2].xAdvance);
  const double ownerEnd = glyphs[2].xPosition + glyphs[2].xAdvance;

  EXPECT_THAT(innerGap, Gt(0.0));
  EXPECT_THAT(std::vector<double>({ownerEnd - glyphs[0].xPosition, glyphs[3].xPosition - ownerEnd}),
              ElementsAre(DoubleNear(60.0, 1e-9), DoubleNear(naturalGap + innerGap, 1e-9)));
}

TEST(TextEngineScriptedTest, TextPathLengthRejectsUnrepresentableGlyphScale) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.pathSpline = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(100.0, 0.0)).build();
  span.textLength = Lengthd(1e300, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(std::move(span));
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(testing::Each(AllOf(
                        GlyphXAdvanceIs(DoubleEq(10.0)), GlyphStretchScaleXIs(FloatEq(1.0f)))))));
}

TEST(TextEngineScriptedTest, HorizontalTextPathIgnoresYOnlyPositioningAndChunkBoundaries) {
  for (const bool scripted : {true, false}) {
    SCOPED_TRACE(scripted);
    const auto expected = LayoutParsedTextPath(
        R"(<textPath href="#p" startOffset="150" text-anchor="middle">A<tspan>BC</tspan>D</textPath>)",
        scripted);
    const auto actual = LayoutParsedTextPath(
        R"(<textPath href="#p" startOffset="150" text-anchor="middle">A<tspan y="20 40">BC</tspan>D</textPath>)",
        scripted);
    ASSERT_THAT(actual, SizeIs(expected.size()));
    for (size_t i = 0; i < actual.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_THAT(actual[i], AllOf(GlyphXPositionIs(DoubleNear(expected[i].xPosition, 1e-6)),
                                   GlyphYPositionIs(DoubleNear(expected[i].yPosition, 1e-6)),
                                   GlyphXAdvanceIs(DoubleNear(expected[i].xAdvance, 1e-6))));
    }
  }
}

TEST(TextEngineScriptedTest, HorizontalTextPathCarriesDyAlongThePathNormal) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="120" lengthAdjust="spacingAndGlyphs">A<tspan dy="10">B</tspan><tspan dy="-5">C</tspan>D</textPath>)",
      /*scripted=*/true, "M0 0H40V200");
  EXPECT_THAT(glyphs, ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)),
                                        GlyphYPositionIs(DoubleNear(0.0, 1e-6))),
                                  AllOf(GlyphXPositionIs(DoubleNear(30.0, 1e-6)),
                                        GlyphYPositionIs(DoubleNear(-400.0 / 49.0, 1e-6)),
                                        GlyphRotateDegreesIs(DoubleNear(90.0, 1e-6))),
                                  AllOf(GlyphXPositionIs(DoubleNear(35.0, 1e-6)),
                                        GlyphYPositionIs(DoubleNear(1160.0 / 49.0, 1e-6))),
                                  AllOf(GlyphXPositionIs(DoubleNear(35.0, 1e-6)),
                                        GlyphYPositionIs(DoubleNear(2720.0 / 49.0, 1e-6)))));
}

TEST(TextEngineScriptedTest, TextPathDyResetsForConsecutivePaths) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p"><tspan dy="10">A</tspan></textPath><textPath href="#p">B</textPath>)");
  EXPECT_THAT(glyphs, ElementsAre(GlyphYPositionIs(DoubleNear(10.0, 1e-6)),
                                  GlyphYPositionIs(DoubleNear(0.0, 1e-6))));
}

TEST(TextEngineScriptedTest, TextPathAbsoluteXPreservesSameGlyphDx) {
  const auto glyphs =
      LayoutParsedTextPath(R"(<textPath href="#p"><tspan x="50" dx="7">A</tspan></textPath>)");
  EXPECT_THAT(glyphs, ElementsAre(AllOf(GlyphXPositionIs(DoubleNear(57.0, 1e-6)),
                                        GlyphXAdvanceIs(DoubleEq(10.0)))));
}

TEST(TextEngineScriptedTest, TextPathAnchorUsesIntermediateGlyphExtents) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" startOffset="150" text-anchor="middle">A<tspan dx="100">B</tspan><tspan dx="-100">C</tspan></textPath>)");
  EXPECT_THAT(
      glyphs,
      ElementsAre(GlyphXPositionIs(DoubleNear(150.0 - (2.0 * 10.0 + 3.0 + 100.0) / 2.0, 1e-6)),
                  GlyphXPositionIs(DoubleNear(
                      150.0 + 10.0 + 3.0 + 100.0 - (2.0 * 10.0 + 3.0 + 100.0) / 2.0, 1e-6)),
                  GlyphXPositionIs(DoubleNear(
                      150.0 + 2.0 * (10.0 + 3.0) - (2.0 * 10.0 + 3.0 + 100.0) / 2.0, 1e-6))));
}

TEST(TextEngineScriptedTest, TextPathLengthPreservesMultiGlyphClusterOffsets) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager, false, std::nullopt,
                                         /*clusterCombiningMarks=*/true);
  const Path path = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(500.0, 0.0)).build();
  for (const LengthAdjust adjust : {LengthAdjust::Spacing, LengthAdjust::SpacingAndGlyphs}) {
    SCOPED_TRACE(adjust);
    components::ComputedTextComponent text;
    auto span = MakeSpan(
        "q\xCC\x81"
        "B");
    span.pathSpline = path;
    span.textLength = Lengthd(adjust == LengthAdjust::Spacing ? 61.0 : 42.0, Lengthd::Unit::None);
    span.lengthAdjust = adjust;
    text.spans.push_back(std::move(span));
    const auto runs = engine.layout(text, MakeTextParams(20.0));
    const double scale = adjust == LengthAdjust::Spacing ? 1.0 : 2.0;
    const double nextOrigin = adjust == LengthAdjust::Spacing ? 51.0 : 22.0;
    EXPECT_THAT(
        runs,
        ElementsAre(RunGlyphsAre(ElementsAre(
            AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-6)), GlyphYPositionIs(DoubleNear(0.0, 1e-6)),
                  GlyphXAdvanceIs(DoubleEq(10.0 * scale))),
            AllOf(GlyphXPositionIs(DoubleNear(3.0 * scale, 1e-6)),
                  GlyphYPositionIs(DoubleNear(-4.0, 1e-6)), GlyphXAdvanceIs(DoubleEq(0.0)),
                  Field("cluster", &TextGlyph::cluster, 0u)),
            GlyphXPositionIs(DoubleNear(nextOrigin, 1e-6))))));
  }
}

TEST(TextEngineScriptedTest, TextPathClusterCoordinatesAndTangentApplyOnce) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager, false, std::nullopt,
                                         /*clusterCombiningMarks=*/true);
  components::ComputedTextComponent text;
  auto span = MakeSpan("q\xCC\x81");
  span.pathSpline = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(0.0, 200.0)).build();
  span.pathStartOffset = 20.0;
  span.textLength = Lengthd(100.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::Spacing;
  span.xList = {Lengthd(10.0, Lengthd::Unit::None)};
  span.dxList = {Lengthd(2.0, Lengthd::Unit::None)};
  span.dyList = {Lengthd(5.0, Lengthd::Unit::None)};
  text.spans.push_back(std::move(span));
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  EXPECT_THAT(
      runs,
      ElementsAre(RunGlyphsAre(ElementsAre(
          AllOf(GlyphXPositionIs(DoubleNear(-5.0, 1e-6)), GlyphYPositionIs(DoubleNear(32.0, 1e-6)),
                GlyphRotateDegreesIs(DoubleNear(90.0, 1e-6))),
          AllOf(GlyphXPositionIs(DoubleNear(-1.0, 1e-6)), GlyphYPositionIs(DoubleNear(35.0, 1e-6)),
                GlyphRotateDegreesIs(DoubleNear(90.0, 1e-6)), GlyphIndexIs(Not(Eq(0))))))));
}

TEST(TextEngineScriptedTest, TextPathClusterSpacingRespectsNestedLengthOwners) {
  const auto glyphs = LayoutParsedTextPath(
      R"(<textPath href="#p" textLength="100">A<tspan textLength="40">q&#x301;B</tspan>C</textPath>)",
      /*scripted=*/true, "M0 0H500", /*clusterCombiningMarks=*/true);
  // The child's 19px trailing gap after B is fixed advance that the outer owner's two gaps no
  // longer have to supply, so each of them shrinks by 9.5px. This follows Donner's fixed-child
  // model for nested length owners.
  EXPECT_THAT(
      glyphs,
      ElementsAre(
          GlyphXPositionIs(DoubleNear(0.0, 1e-6)), GlyphXPositionIs(DoubleNear(20.5, 1e-6)),
          AllOf(GlyphXPositionIs(DoubleNear(23.5, 1e-6)), GlyphYPositionIs(DoubleNear(-4.0, 1e-6))),
          GlyphXPositionIs(DoubleNear(50.5, 1e-6)), GlyphXPositionIs(DoubleNear(90.0, 1e-6))));
}

TEST(TextEngineScriptedTest, TextPathLengthAdjustsAdvancesBeforeCurvedPlacement) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  const entt::entity textPathEntity = registry.create();
  const Path path = PathBuilder()
                        .moveTo(Vector2d(0.0, 0.0))
                        .lineTo(Vector2d(20.0, 0.0))
                        .lineTo(Vector2d(20.0, 100.0))
                        .build();

  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.pathSpline = path;
  span.textPathSourceEntity = textPathEntity;
  span.textLength = Lengthd(42.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(AllOf(RunOnPathIs(Eq(true)),
                                      RunGlyphsAre(ElementsAre(
                                          AllOf(GlyphXPositionIs(DoubleNear(0.0, 1e-9)),
                                                GlyphXAdvanceIs(DoubleNear(20.0, 1e-9)),
                                                GlyphStretchScaleXIs(FloatEq(2.0f))),
                                          AllOf(GlyphXPositionIs(DoubleNear(20.0, 1e-9)),
                                                GlyphYPositionIs(DoubleNear(2.0, 1e-9)),
                                                GlyphRotateDegreesIs(DoubleNear(90.0, 1e-9))))))));
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

TEST(TextEngineScriptedTest, RtlTextLengthDoesNotRepeatConsumedSpanStartPosition) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager, /*reverseClusters=*/true);

  components::ComputedTextComponent text;
  auto span = MakeSpan("ABC");
  span.xList = {Lengthd(20.0, Lengthd::Unit::None)};
  span.startsNewChunk = true;
  span.fontKerning = FontKerning::None;
  span.textLength = Lengthd(60.0, Lengthd::Unit::None);
  span.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphXPositionIs(DoubleEq(20.0)), GlyphXAdvanceIs(DoubleEq(20.0))),
                        AllOf(GlyphXPositionIs(DoubleEq(40.0)), GlyphXAdvanceIs(DoubleEq(20.0))),
                        GlyphXPositionIs(DoubleEq(60.0))))));
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

  // Latin glyphs are set sideways in vertical text, so their outlines stretch along their own x
  // axis, which the quarter turn maps onto the vertical inline axis.
  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(
                  AllOf(GlyphYPositionIs(DoubleEq(0.0)), GlyphYAdvanceIs(DoubleEq(20.0)),
                        GlyphStretchScaleXIs(FloatEq(2.0f)), GlyphStretchScaleYIs(FloatEq(1.0f))),
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
                     size_t byteOffset, size_t byteLength, bool isVertical, FontVariant fontVariant,
                     bool forceLogicalOrder) const override {
    ShapedRun run =
        ScriptedTextBackend::shapeRun(font, fontSizePx, spanText, byteOffset, byteLength,
                                      isVertical, fontVariant, forceLogicalOrder);
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
  mutable std::size_t bitmapDecodeCalls = 0;
  Path glyphOutline(FontHandle /*font*/, int /*glyphIndex*/, float /*scale*/) const override {
    return Path();
  }

  std::optional<BitmapGlyphMetrics> bitmapGlyphMetrics(FontHandle font, int glyphIndex,
                                                       float scale) const override {
    if (glyphIndex == 'A' % 997 + 1) {
      return BitmapGlyphMetrics{.font = font,
                                .glyphIndex = glyphIndex,
                                .width = 4,
                                .height = 4,
                                .rgbaBytes = 64,
                                .bearingX = 1.0,
                                .bearingY = 2.0,
                                .scale = 0.5,
                                .requestedScale = scale};
    }
    return BitmapGlyphMetrics{.font = font, .glyphIndex = glyphIndex, .requestedScale = scale};
  }

  std::optional<BitmapGlyph> bitmapGlyph(FontHandle /*font*/, int glyphIndex,
                                         float /*scale*/) const override {
    ++bitmapDecodeCalls;
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

class MixedBitmapOutlineBackend : public BitmapOutlineBackend {
public:
  Path glyphOutline(FontHandle font, int glyphIndex, float scale) const override {
    return glyphIndex == 'A' % 997 + 1 ? Path()
                                       : ScriptedTextBackend::glyphOutline(font, glyphIndex, scale);
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
  source.trusted = true;

  css::FontFace face;
  face.familyName = RcString(familyName);
  face.sources.push_back(std::move(source));
  fontManager.addFontFace(face);
  return fontManager.findFont(RcString(familyName));
}

/// Minimal CFF sfnt bytes that pass the bounded directory check but fail stb_truetype parsing.
std::shared_ptr<const std::vector<uint8_t>> MakeUnparseableOutlineFontData() {
  std::vector<uint8_t> data = {'O',  'T',  'T',  'O',  // OpenType CFF magic.
                               0x00, 0x01,             // numTables = 1
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const char tag[4] = {'C', 'F', 'F', ' '};
  data.insert(data.end(), tag, tag + 4);
  for (int i = 0; i < 4; ++i) {
    data.push_back(0);  // Checksum.
  }
  data.insert(data.end(), {0x00, 0x00, 0x00, 0x1C});  // Offset = 28.
  data.insert(data.end(), {0x00, 0x00, 0x00, 0x01});  // Length = 1.
  data.push_back(0);
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

TEST(TextEngineScriptedTest, PrepareForElementCapsInitialStylesheetFontFaces) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg">)";
  for (std::size_t i = 0; i < components::ResourceManagerContext::kMaximumStylesheetFontFaces + 1;
       ++i) {
    source += "<style>@font-face{font-family:F" + std::to_string(i) + ";src:local(F" +
              std::to_string(i) + ")}</style>";
  }
  source += R"(<text id="target">x</text></svg>)";

  ParseWarningSink parseWarnings;
  ParseResult<SVGDocument> parsed = parser::SVGParser::ParseSVG(source, parseWarnings);
  ASSERT_TRUE(parsed.hasResult());
  SVGDocument document = std::move(parsed).result();
  std::optional<SVGElement> target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  Registry& registry = document.registry();
  auto& resourceManager = registry.ctx().get<components::ResourceManagerContext>();
  ASSERT_THAT(resourceManager.fontFaces(), IsEmpty());

  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  ParseWarningSink renderWarnings;
  engine.prepareForElement(target->unsafeEntityHandle(), renderWarnings);

  EXPECT_THAT(resourceManager.fontFaces(),
              SizeIs(components::ResourceManagerContext::kMaximumStylesheetFontFaces));
  EXPECT_EQ(resourceManager.stylesheetFontFaceCountForTesting(),
            components::ResourceManagerContext::kMaximumStylesheetFontFaces);
  EXPECT_TRUE(resourceManager.stylesheetFontFaceLimitRejected());
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
    registry.emplace<donner::components::TreeComponent>(entity, xml::XMLQualifiedNameRef("tspan"));
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
  // The run keeps the span's face; the glyph it cannot render carries the covering face.
  EXPECT_EQ(runs[0].font, fontManager.fallbackFont());
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(AllOf(GlyphIndexIs(Gt(0)), Field("font", &TextGlyph::font, altFace))));
}

class SynthesisCoverageBackend : public CoverageProbeBackend {
public:
  FontHandle nativeFace;
  bool hasSmallCapsFeature(FontHandle font) const override { return font == nativeFace; }
};

TEST(TextEngineScriptedTest, SmallCapsSynthesisUsesEachFallbackFacesCapability) {
  for (bool nativeFallback : {false, true}) {
    SCOPED_TRACE(nativeFallback);
    Registry registry;
    FontManager manager(registry);
    const FontHandle alternate = RegisterFallbackBackedFace(manager, "CapsFallback");
    ASSERT_TRUE(static_cast<bool>(alternate));
    auto backend = std::make_unique<SynthesisCoverageBackend>();
    backend->coveringFont = alternate;
    backend->nativeFace = nativeFallback ? alternate : manager.fallbackFont();
    TextEngine engine(manager, registry, std::move(backend));
    components::ComputedTextComponent text;
    auto span = MakeSpan("A\xC3\xA9");
    span.fontVariant = FontVariant::SmallCaps;
    span.fontSynthesisSmallCaps = FontSynthesisSmallCaps::None;
    text.spans.push_back(std::move(span));
    const auto runs = engine.layout(text, MakeTextParams(20));
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs.front().glyphs, SizeIs(2));
    EXPECT_FLOAT_EQ(runs.front().glyphs[0].fontSizeScale, nativeFallback ? 1.0f : 0.8f);
    EXPECT_FLOAT_EQ(runs.front().glyphs[1].fontSizeScale, nativeFallback ? 0.8f : 1.0f);
    EXPECT_EQ(runs.front().glyphs[1].font, alternate);
    EXPECT_EQ(runs.front().glyphs[0].cluster, 0u);
    EXPECT_EQ(runs.front().glyphs[1].cluster, 1u);
  }
}

TEST(TextEngineScriptedTest, SmallCapsSynthesisPreservesCompatibleNativeKerning) {
  for (bool native : {false, true}) {
    Registry registry;
    FontManager manager(registry);
    auto backend = std::make_unique<ScriptedTextBackend>();
    backend->nativeSmallCaps = native;
    TextEngine engine(manager, registry, std::move(backend));
    components::ComputedTextComponent text;
    auto first = MakeSpan("a");
    first.fontVariant = FontVariant::SmallCaps;
    auto second = MakeSpan("b");
    second.startsNewChunk = false;
    second.fontVariant = FontVariant::SmallCaps;
    second.fontSynthesisSmallCaps = FontSynthesisSmallCaps::None;
    text.spans.push_back(std::move(first));
    text.spans.push_back(std::move(second));
    const auto runs = engine.layout(text, MakeTextParams(20));
    ASSERT_THAT(runs, SizeIs(2));
    ASSERT_THAT(runs[1].glyphs, SizeIs(1));
    EXPECT_DOUBLE_EQ(runs[1].glyphs.front().xPosition, native ? 13.0 : 10.0);
  }
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

TEST(TextEngineScriptedTest, UnrepresentableSizeAdjustmentDoesNotReachShaping) {
  for (double adjustment : {1e40, 1e308}) {
    SCOPED_TRACE(adjustment);
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    text.spans.push_back(MakeSpan("AB"));
    auto params = MakeTextParams(64.0);
    params.fontSizeAdjust = adjustment;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(IsEmpty())));
    EXPECT_THAT(runs.front().usedFontSizePx, FloatEq(0.0f));
  }
}

TEST(TextEngineScriptedTest, UnknownXHeightDoesNotGuessSizeAdjustment) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->xHeight = 0;
  TextEngine engine(fontManager, registry, std::move(backend));
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("x"));
  auto params = MakeTextParams(20.0);
  params.fontSizeAdjust = 0.3;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  EXPECT_FLOAT_EQ(runs.front().usedFontSizePx, 20.0f);
}

TEST(TextEngineScriptedTest, ZeroAdjustmentDoesNotRequireAnXHeightMetric) {
  Registry registry;
  FontManager fontManager(registry);
  auto shapedSize = std::make_shared<float>(-1.0f);
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->xHeight = 0;
  backend->shapedFontSizePx = shapedSize;
  TextEngine engine(fontManager, registry, std::move(backend));
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("x"));
  auto params = MakeTextParams(20.0);
  params.fontSizeAdjust = 0.0;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  EXPECT_EQ(runs.front().usedFontSizePx, 0.0f);
  EXPECT_THAT(runs.front().glyphs, ElementsAre(GlyphXAdvanceIs(DoubleEq(0.0))));
  EXPECT_EQ(*shapedSize, -1.0f);
}

TEST(TextEngineScriptedTest, SizeAdjustmentUsesExactDesignUnitAspect) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->xHeight = 536;
  TextEngine engine(fontManager, registry, std::move(backend));
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("Text"));
  auto params = MakeTextParams(64.0);
  params.fontSizeAdjust = 0.3;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  const float expected = static_cast<float>(64.0 * 0.3 / (536.0 / 1000.0));
  EXPECT_EQ(runs.front().usedFontSizePx, expected);
}

TEST(TextEngineScriptedTest, FontSizeAdjustChangesUsedFontSizeFromXHeight) {
  Registry registry;
  FontManager fontManager(registry);
  auto shapedFontSizePx = std::make_shared<float>();
  auto backend = std::make_unique<ScriptedTextBackend>();
  backend->shapedFontSizePx = shapedFontSizePx;
  TextEngine engine(fontManager, registry, std::move(backend));

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));

  TextLayoutParams params = MakeTextParams(20.0);
  params.fontSizeAdjust = 0.3;
  const auto runs = engine.layout(text, params);

  ASSERT_THAT(runs, ElementsAre(RunGlyphsAre(SizeIs(1))));
  EXPECT_FLOAT_EQ(*shapedFontSizePx, 12.0f);
}

TEST(TextEngineScriptedTest, SpanKerningCanOverrideDisabledParent) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);
  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.fontKerning = FontKerning::Normal;
  text.spans.push_back(std::move(span));
  TextLayoutParams params = MakeTextParams(20.0);
  params.fontKerning = FontKerning::None;
  const auto runs = engine.layout(text, params);
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(0.0)),
                                                         GlyphXPositionIs(DoubleEq(11.0))))));
}

TEST(TextEngineScriptedTest, FontKerningNoneSuppressesWithinRunKerning) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto span = MakeSpan("AB");
  span.fontKerning = FontKerning::None;
  text.spans.push_back(std::move(span));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(0.0)),
                                                         GlyphXPositionIs(DoubleEq(10.0))))));
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

TEST(TextEngineScriptedTest, CrossSpanKerningPreservesSizeChangesButRejectsVariantChanges) {
  for (bool differentSize : {false, true}) {
    SCOPED_TRACE(differentSize);
    Registry registry;
    FontManager fontManager(registry);
    TextEngine engine = MakeScriptedEngine(registry, fontManager);
    components::ComputedTextComponent text;
    text.spans.push_back(MakeSpan("A"));
    auto continuation = MakeSpan("V");
    continuation.startsNewChunk = false;
    if (differentSize) {
      continuation.fontSize = Lengthd(30.0, Lengthd::Unit::Px);
    } else {
      continuation.fontVariant = FontVariant::SmallCaps;
    }
    text.spans.push_back(std::move(continuation));
    const auto runs = engine.layout(text, MakeTextParams(20.0));
    ASSERT_THAT(runs, SizeIs(2));
    ASSERT_EQ(runs[0].font, runs[1].font);
    EXPECT_THAT(runs[1].glyphs,
                ElementsAre(GlyphXPositionIs(DoubleEq(differentSize ? 13.0 : 10.0))));
  }
}

TEST(TextEngineScriptedTest, ZeroUsedSizeDoesNotBridgeCrossSpanKerning) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));
  auto middle = MakeSpan("B");
  middle.startsNewChunk = false;
  middle.fontSizeAdjust.emplace(0.0);
  text.spans.push_back(std::move(middle));
  auto continuation = MakeSpan("V");
  continuation.startsNewChunk = false;
  text.spans.push_back(std::move(continuation));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  ASSERT_THAT(
      runs, ElementsAre(RunGlyphsAre(SizeIs(1)), RunGlyphsAre(SizeIs(1)), RunGlyphsAre(SizeIs(1))));
  EXPECT_THAT(runs[0].usedFontSizePx, FloatEq(20.0f));
  EXPECT_THAT(runs[1].usedFontSizePx, FloatEq(0.0f));
  EXPECT_THAT(runs[2].usedFontSizePx, FloatEq(20.0f));
  EXPECT_THAT(runs[1].font, Eq(runs[0].font));
  EXPECT_THAT(runs[2].font, Eq(runs[0].font));
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphXAdvanceIs(DoubleEq(10.0)))));
  EXPECT_THAT(runs[1].glyphs,
              ElementsAre(AllOf(GlyphIndexIs(Eq(0)), GlyphXPositionIs(DoubleEq(10.0)),
                                GlyphXAdvanceIs(DoubleEq(0.0)))));
  EXPECT_THAT(runs[2].glyphs, ElementsAre(GlyphXPositionIs(DoubleEq(10.0))));
}

TEST(TextEngineScriptedTest, EmptySpanPreservesCrossSpanKerning) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));
  auto middle = MakeSpan("");
  middle.startsNewChunk = false;
  middle.fontSizeAdjust.emplace(0.0);
  text.spans.push_back(std::move(middle));
  auto continuation = MakeSpan("V");
  continuation.startsNewChunk = false;
  text.spans.push_back(std::move(continuation));

  const auto runs = engine.layout(text, MakeTextParams(20.0));

  ASSERT_THAT(
      runs, ElementsAre(RunGlyphsAre(SizeIs(1)), RunGlyphsAre(IsEmpty()), RunGlyphsAre(SizeIs(1))));
  EXPECT_THAT(runs[0].usedFontSizePx, FloatEq(20.0f));
  EXPECT_THAT(runs[1].usedFontSizePx, FloatEq(0.0f));
  EXPECT_THAT(runs[2].usedFontSizePx, FloatEq(20.0f));
  EXPECT_THAT(runs[1].font, Eq(runs[0].font));
  EXPECT_THAT(runs[2].font, Eq(runs[0].font));
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphXAdvanceIs(DoubleEq(10.0)))));
  EXPECT_THAT(runs[2].glyphs, ElementsAre(GlyphXPositionIs(DoubleEq(13.0))));
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
  EXPECT_THAT(runs,
              ElementsAre(RunGlyphsAre(ElementsAre(
                  AllOf(GlyphRotateDegreesIs(DoubleEq(7.0)), GlyphYPositionIs(DoubleEq(3.0))),
                  AllOf(GlyphRotateDegreesIs(DoubleEq(7.0)), GlyphYPositionIs(DoubleEq(17.0)))))));
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

  // A sibling span without textLength is not stretched, but inherits the adjusted current position.
  auto plainSpan = MakeSpan("C");
  plainSpan.startsNewChunk = false;
  text.spans.push_back(std::move(plainSpan));

  TextLayoutParams params = MakeTextParams(20.0);
  params.writingMode = WritingMode::VerticalRl;
  const auto runs = engine.layout(text, params);

  // Natural length 22px (10px advance, 2px kern, 10px advance) stretched to 40px moves
  // the second glyph down by the 18px gap difference. The plain span starts from the adjusted
  // 40px current position, plus the 18px gap that spacing accrues after the last character,
  // plus the 4px cross-span kern.
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(0.0)),
                                                         GlyphYPositionIs(DoubleEq(30.0)))),
                                RunGlyphsAre(ElementsAre(GlyphYPositionIs(DoubleEq(62.0))))));
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

  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(
                        AllOf(GlyphXPositionIs(DoubleEq(0.0)), GlyphYPositionIs(DoubleEq(0.0)))))));
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

// `visibility: hidden` suppresses painting, not layout, so a hidden span takes part in its chunk's
// text-anchor measurement. The chunk's anchor comes from its first span, and the hidden span's
// glyphs count toward the measured width.
TEST(TextEngineScriptedTest, HiddenFirstSpanAnchorsItsChunk) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  components::ComputedTextComponent text;
  auto hidden = MakeSpan("AB");
  hidden.visibility = Visibility::Hidden;
  hidden.textAnchor = TextAnchor::Start;

  auto visible = MakeSpan("CD");
  visible.startsNewChunk = false;
  visible.textAnchor = TextAnchor::Middle;

  text.spans.push_back(std::move(visible));
  const auto centredAlone = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(centredAlone.at(0).glyphs, SizeIs(2));
  const double centredAloneX = centredAlone.at(0).glyphs.front().xPosition;

  text.spans.insert(text.spans.begin(), std::move(hidden));
  const auto runs = engine.layout(text, MakeTextParams(20.0));
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs.at(0).glyphs, SizeIs(2));
  ASSERT_THAT(runs.at(1).glyphs, SizeIs(2));

  // The chunk takes the first span's `start` anchor, so the pen runs forward from the origin: the
  // hidden span occupies the front of the chunk and the visible span follows it, instead of the
  // visible span being centred on the origin by its own anchor.
  EXPECT_THAT(runs.at(0).glyphs.front().xPosition, Gt(centredAloneX));
  EXPECT_THAT(runs.at(1).glyphs.front().xPosition, Gt(runs.at(0).glyphs.back().xPosition));
}

// A centred chunk measures the hidden span's advance too, so the whole chunk including the hidden
// run is what gets centred on the anchor point.
TEST(TextEngineScriptedTest, HiddenSpanCountsTowardChunkCentring) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const auto layoutFirstGlyphX = [&](std::optional<Visibility> trailingVisibility) {
    components::ComputedTextComponent text;
    auto visible = MakeSpan("AB");
    visible.xList = {Lengthd(100.0, Lengthd::Unit::None)};
    visible.textAnchor = TextAnchor::Middle;
    text.spans.push_back(std::move(visible));

    if (trailingVisibility.has_value()) {
      auto trailing = MakeSpan("CDEF");
      trailing.startsNewChunk = false;
      trailing.visibility = trailingVisibility.value();
      text.spans.push_back(std::move(trailing));
    }

    const auto runs = engine.layout(text, MakeTextParams(20.0));
    return runs.at(0).glyphs.at(0).xPosition;
  };

  const double withHiddenTrailing = layoutFirstGlyphX(Visibility::Hidden);

  // Centring the chunk with the hidden span in it shifts its start left exactly as a visible span
  // of the same width would, and further left than the two visible glyphs alone.
  EXPECT_THAT(withHiddenTrailing, DoubleNear(layoutFirstGlyphX(Visibility::Visible), 0.001));
  EXPECT_THAT(withHiddenTrailing, Lt(layoutFirstGlyphX(std::nullopt)));
}

// Inline-size wrapping sees the hidden span's words, so they occupy line width and push the
// following visible word onto the next line.
TEST(TextEngineScriptedTest, HiddenSpanOccupiesInlineSizeLineWidth) {
  Registry registry;
  FontManager fontManager(registry);
  TextEngine engine = MakeScriptedEngine(registry, fontManager);

  const auto lineCount = [&](Visibility firstVisibility) {
    components::ComputedTextComponent text;
    auto first = MakeSpan("aa ");
    first.visibility = firstVisibility;

    auto second = MakeSpan("bb");
    second.startsNewChunk = false;

    text.spans.push_back(std::move(first));
    text.spans.push_back(std::move(second));

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
    return baselines.size();
  };

  EXPECT_THAT(lineCount(Visibility::Hidden), Eq(lineCount(Visibility::Visible)));
  EXPECT_THAT(lineCount(Visibility::Hidden), Eq(2u));
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
  EXPECT_THAT(runs, ElementsAre(RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(-21.0)),
                                                         GlyphXPositionIs(DoubleEq(-10.0)))),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(0.0)))),
                                RunGlyphsAre(IsEmpty()),
                                RunGlyphsAre(ElementsAre(GlyphXPositionIs(DoubleEq(70.0))))));
}

TEST(TextEngineScriptedTest, DomUtf16SupplementaryUnitsShareGlyphGeometry) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  std::string text;
  Utf8::Append(U'\U0001F600', std::back_inserter(text));
  auto span = MakeSpan(text);
  span.xList = {Lengthd(10.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(20.0, Lengthd::Unit::None)};
  const Entity root = MakeTextRootWithSpan(registry, text, std::move(span));
  const EntityHandle handle(registry, root);

  EXPECT_EQ(engine.getNumberOfChars(handle), 2);
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 0), Eq(Vector2d(12.0, 23.0)));
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 1), Eq(Vector2d(12.0, 23.0)));
  EXPECT_THAT(engine.getEndPositionOfChar(handle, 1), Eq(engine.getEndPositionOfChar(handle, 0)));
  EXPECT_THAT(engine.getExtentOfChar(handle, 0), Eq(Box2d::FromXYWH(12.0, 15.0, 6.0, 8.0)));
  EXPECT_THAT(engine.getExtentOfChar(handle, 1), Eq(Box2d::FromXYWH(12.0, 15.0, 6.0, 8.0)));
  EXPECT_EQ(engine.getCharNumAtPosition(handle, {15.0, 19.0}), 0);
  EXPECT_THAT(engine.getSubStringLength(handle, 0, 2), DoubleEq(std::hypot(10.0, 12.0)));
  EXPECT_THAT(engine.getSubStringLength(handle, 1, 1), DoubleEq(0.0));
}

TEST(TextEngineScriptedTest, DomUtf16FollowingCharacterKeepsCodepointPositioning) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine = MakeScriptedEngine(registry, manager);
  std::string text("A");
  Utf8::Append(U'\U0001F600', std::back_inserter(text));
  text += "B";
  auto span = MakeSpan(text);
  span.xList = {Lengthd(10.0, Lengthd::Unit::None), Lengthd(30.0, Lengthd::Unit::None),
                Lengthd(70.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(20.0, Lengthd::Unit::None)};
  const Entity root = MakeTextRootWithSpan(registry, text, std::move(span));
  const EntityHandle handle(registry, root);

  EXPECT_EQ(engine.getNumberOfChars(handle), 4);
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 1), Eq(Vector2d(32.0, 23.0)));
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 2), Eq(Vector2d(32.0, 23.0)));
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 3), Eq(Vector2d(70.0, 20.0)));
  EXPECT_THAT(engine.getExtentOfChar(handle, 1), Eq(Box2d::FromXYWH(32.0, 15.0, 6.0, 8.0)));
  EXPECT_THAT(engine.getExtentOfChar(handle, 2), Eq(Box2d::FromXYWH(32.0, 15.0, 6.0, 8.0)));
  EXPECT_THAT(engine.getExtentOfChar(handle, 3), Eq(Box2d::FromXYWH(70.0, 12.0, 6.0, 8.0)));
  EXPECT_EQ(engine.getCharNumAtPosition(handle, {73.0, 16.0}), 3);
  EXPECT_THAT(engine.getSubStringLength(handle, 1, 2), DoubleEq(std::hypot(10.0, 12.0)));
  EXPECT_THAT(engine.getSubStringLength(handle, 2, 1), DoubleEq(0.0));
  EXPECT_THAT(engine.getSubStringLength(handle, 3, 1), DoubleEq(std::hypot(10.0, 12.0)));
  EXPECT_THAT(engine.getComputedTextLength(handle), DoubleEq(3.0 * std::hypot(10.0, 12.0)));
}

TEST(TextEngineScriptedTest, DomUtf16CountsCombiningAndJoinerUnits) {
  for (const char32_t middle : {U'\u0301', U'\u200D'}) {
    SCOPED_TRACE(static_cast<uint32_t>(middle));
    Registry registry;
    FontManager manager(registry);
    TextEngine engine = MakeScriptedEngine(registry, manager);
    std::string text("A");
    Utf8::Append(middle, std::back_inserter(text));
    text += "B";
    const Entity root = MakeTextRootWithSpan(registry, text, MakeSpan(text));
    EXPECT_EQ(engine.getNumberOfChars(EntityHandle(registry, root)), 3);
  }
}

TEST(TextEngineScriptedTest, DomUtf16CombiningAndJoinerKeepFollowingGlyphIdentity) {
  for (const char32_t middle : {U'\u0301', U'\u200D'}) {
    SCOPED_TRACE(static_cast<uint32_t>(middle));
    Registry registry;
    FontManager manager(registry);
    TextEngine engine = MakeScriptedEngine(registry, manager);
    std::string text("A");
    Utf8::Append(middle, std::back_inserter(text));
    text += "B";
    auto span = MakeSpan(text);
    span.xList = {Lengthd(10.0, Lengthd::Unit::None), Lengthd(40.0, Lengthd::Unit::None),
                  Lengthd(70.0, Lengthd::Unit::None)};
    span.yList = {Lengthd(20.0, Lengthd::Unit::None)};
    const Entity root = MakeTextRootWithSpan(registry, text, std::move(span));
    const EntityHandle handle(registry, root);

    const auto& geometry = engine.ensureComputedTextGeometryComponent(handle);
    ASSERT_THAT(geometry.runs, SizeIs(1));
    ASSERT_THAT(geometry.runs[0].glyphs, SizeIs(3));
    const TextGlyph& following = geometry.runs[0].glyphs.back();
    EXPECT_EQ(following.cluster, middle == U'\u0301' ? 3u : 4u);
    EXPECT_THAT(following.xPosition, DoubleEq(70.0));
    EXPECT_THAT(following.yPosition, DoubleEq(20.0));
    EXPECT_THAT(engine.glyphOutline(geometry.runs[0].font, following.glyphIndex, 1.0f).bounds(),
                Eq(Box2d::FromXYWH(0.0, -8.0, 6.0, 8.0)));

    EXPECT_EQ(engine.getNumberOfChars(handle), 3);
    EXPECT_THAT(engine.getStartPositionOfChar(handle, 2), Eq(Vector2d(70.0, 20.0)));
    EXPECT_THAT(engine.getEndPositionOfChar(handle, 2), Eq(Vector2d(80.0, 32.0)));
    EXPECT_THAT(engine.getExtentOfChar(handle, 2), Eq(Box2d::FromXYWH(70.0, 12.0, 6.0, 8.0)));
    EXPECT_EQ(engine.getCharNumAtPosition(handle, {73.0, 16.0}), 2);
    EXPECT_THAT(engine.getSubStringLength(handle, 2, 1), DoubleEq(std::hypot(10.0, 12.0)));
  }
}

TEST(TextEngineScriptedTest, GeometryCacheFallsBackToBitmapGlyphExtents) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<BitmapOutlineBackend>();
  const auto* observedBackend = backend.get();
  TextEngine engine(fontManager, registry, std::move(backend));

  const Entity root = MakeTextRootWithSpan(registry, "AB", MakeSpan("AB"));
  const EntityHandle rootHandle(registry, root);

  // No vector outlines anywhere.
  EXPECT_THAT(engine.computedGlyphPaths(rootHandle), IsEmpty());

  // 'A' resolves to a 4x4 bitmap at scale 0.5 with bearing (1, 2).
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 0), Box2d::FromXYWH(1.0, -2.0, 2.0, 2.0));
  // 'B' resolves to a zero-sized bitmap: its extent stays empty.
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 1), Box2d());
  EXPECT_EQ(observedBackend->bitmapDecodeCalls, 0u)
      << "Text bounds must use metadata without materializing glyph pixels.";

  EXPECT_EQ(engine.getNumberOfChars(rootHandle), 2);
  // End position advances by the scripted (10, 12) glyph advance.
  EXPECT_EQ(engine.getEndPositionOfChar(rootHandle, 0), Vector2d(10.0, 12.0));

  // Out-of-range character queries return empty values.
  EXPECT_EQ(engine.getEndPositionOfChar(rootHandle, 9), Vector2d());
  EXPECT_EQ(engine.getExtentOfChar(rootHandle, 9), Box2d());
  EXPECT_DOUBLE_EQ(engine.getRotationOfChar(rootHandle, 9), 0.0);
}

TEST(TextEngineScriptedTest, BitmapGeometryRotatesAboutItsNonzeroGlyphOrigin) {
  struct Case {
    double rotation;
    Box2d expected;
    Vector2d inside;
    Vector2d outside;
  };
  const std::array<Case, 3> cases = {{
      {90.0, Box2d::FromXYWH(10.0, 21.0, 2.0, 2.0), {11.0, 22.0}, {12.0, 19.0}},
      {-90.0, Box2d::FromXYWH(8.0, 17.0, 2.0, 2.0), {9.0, 18.0}, {12.0, 19.0}},
      {30.0,
       Box2d({10.866025403784439, 18.767949192431123}, {13.598076211353316, 21.5}),
       {12.0, 20.5},
       {13.5, 18.0}},
  }};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.rotation);
    Registry registry;
    FontManager manager(registry);
    auto backend = std::make_unique<BitmapOutlineBackend>();
    const auto* observedBackend = backend.get();
    TextEngine engine(manager, registry, std::move(backend));
    auto span = MakeSpan("A");
    span.xList = {Lengthd(10.0, Lengthd::Unit::None)};
    span.yList = {Lengthd(20.0, Lengthd::Unit::None)};
    span.rotateList = {test.rotation};
    const Entity root = MakeTextRootWithSpan(registry, "A", std::move(span));
    const EntityHandle handle(registry, root);
    EXPECT_THAT(engine.getStartPositionOfChar(handle, 0), testing::Eq(Vector2d(10.0, 20.0)));
    EXPECT_THAT(engine.getExtentOfChar(handle, 0), BitmapBoundsNear(test.expected));
    EXPECT_THAT(engine.computedInkBounds(handle), BitmapBoundsNear(test.expected));
    EXPECT_EQ(engine.getCharNumAtPosition(handle, test.inside), 0);
    EXPECT_EQ(engine.getCharNumAtPosition(handle, test.outside), -1);
    EXPECT_EQ(observedBackend->bitmapDecodeCalls, 0u);
  }
}

TEST(TextEngineScriptedTest, BitmapGeometryStretchesBeforeObliqueRotation) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<BitmapOutlineBackend>();
  const auto* observedBackend = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  auto span = MakeSpan("A");
  span.xList = {Lengthd(10.0, Lengthd::Unit::None)};
  span.yList = {Lengthd(20.0, Lengthd::Unit::None)};
  span.rotateList = {30.0};
  const Entity root = MakeTextRootWithSpan(registry, "A", std::move(span));
  auto& component = registry.get<components::TextComponent>(root);
  component.textLength = Lengthd(20.0, Lengthd::Unit::None);
  component.lengthAdjust = LengthAdjust::SpacingAndGlyphs;
  const EntityHandle handle(registry, root);
  const auto& geometry = engine.ensureComputedTextGeometryComponent(handle);
  ASSERT_THAT(geometry.runs, SizeIs(1));
  ASSERT_THAT(geometry.runs[0].glyphs, SizeIs(1));
  EXPECT_THAT(geometry.runs[0].glyphs[0].stretchScaleX, testing::FloatEq(2.0f));
  const Box2d expected({10.866025403784439, 18.767949192431123}, {15.330127018922193, 22.5});
  EXPECT_THAT(engine.getExtentOfChar(handle, 0), BitmapBoundsNear(expected));
  EXPECT_THAT(engine.computedInkBounds(handle), BitmapBoundsNear(expected));
  EXPECT_EQ(engine.getCharNumAtPosition(handle, {15.0, 21.0}), 0);
  EXPECT_EQ(observedBackend->bitmapDecodeCalls, 0u);
}

TEST(TextEngineScriptedTest, BitmapAndOutlineInkBoundsPreserveSourceSubtrees) {
  Registry registry;
  FontManager manager(registry);
  auto backend = std::make_unique<MixedBitmapOutlineBackend>();
  const auto* observedBackend = backend.get();
  TextEngine engine(manager, registry, std::move(backend));
  const Entity root = MakeTextRootWithSpan(registry, "AB", MakeSpan(""));
  const auto child = [&](const char* text, double x) {
    const Entity entity = registry.create();
    registry.emplace<donner::components::TreeComponent>(entity, xml::XMLQualifiedNameRef("tspan"));
    registry.get<donner::components::TreeComponent>(root).appendChild(registry, entity);
    registry.emplace<components::ComputedStyleComponent>(entity, MakeStyle());
    auto span = MakeSpan(text);
    span.sourceEntity = entity;
    span.xList = {Lengthd(x, Lengthd::Unit::None)};
    span.yList = {Lengthd(0.0, Lengthd::Unit::None)};
    registry.get<components::ComputedTextComponent>(root).spans.push_back(std::move(span));
    return EntityHandle(registry, entity);
  };
  registry.get<components::ComputedTextComponent>(root).spans.clear();
  const EntityHandle bitmap = child("A", 0.0);
  const EntityHandle outline = child("B", 20.0);
  const EntityHandle rootHandle(registry, root);
  EXPECT_THAT(engine.computedInkBounds(bitmap), BitmapBoundsNear(Box2d::FromXYWH(1, -2, 2, 2)));
  EXPECT_THAT(engine.computedInkBounds(outline), BitmapBoundsNear(Box2d::FromXYWH(20, -8, 6, 8)));
  EXPECT_THAT(engine.computedInkBounds(rootHandle), BitmapBoundsNear(Box2d({1, -8}, {26, 0})));
  EXPECT_THAT(engine.computedGlyphPaths(bitmap), IsEmpty());
  EXPECT_THAT(engine.computedGlyphOutlines(bitmap), IsEmpty());
  EXPECT_THAT(engine.computedGlyphPaths(outline), SizeIs(1));
  EXPECT_THAT(engine.computedGlyphOutlines(rootHandle), SizeIs(1));
  EXPECT_EQ(observedBackend->bitmapDecodeCalls, 0u);
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
  source.trusted = true;
  css::FontFace face;
  face.familyName = RcString("BrokenFont");
  face.sources.push_back(std::move(source));
  fontManager.addFontFace(face);

  const std::vector<RcString> families = {RcString("BrokenFont")};
  EXPECT_FALSE(engine.measureChUnitInEm(families).has_value());
}

TEST(TextEngineScriptedTest, WrappedParsedNestedSpanOwnsAnchor) {
  const auto glyphs =
      LayoutParsedTextPath("<tspan text-anchor='end' white-space='pre'>A&#10;BB</tspan>");
  ASSERT_THAT(glyphs, SizeIs(4));
  EXPECT_THAT(glyphs[0].xPosition, DoubleNear(-10, 1e-5));
  EXPECT_THAT(glyphs[2].xPosition, DoubleNear(-21, 1e-5));
}

TEST(TextEngineScriptedTest, WhiteSpaceParsedNewlineRetainsRelativeInlinePosition) {
  const auto glyphs =
      LayoutParsedTextPath("<tspan white-space='pre'>A&#10;<tspan dx='20'>B</tspan></tspan>");
  ASSERT_THAT(glyphs, SizeIs(3));
  EXPECT_THAT(glyphs[2].xPosition, DoubleNear(20, 1e-5));
}

TEST(TextEngineScriptedTest, FullWidthMapsUnicodeWithoutChangingSourceClusters) {
  const auto glyphs = LayoutParsedTextPath(
      "<tspan style='text-transform:full-width'>A1!\xef\xbd\xb6\xef\xbe\x9e"
      "\xc3\xa9\xf0\x9f\x98\x80</tspan>");
  const auto mapped = [](uint32_t cp, uint32_t offset) {
    return AllOf(GlyphIndexIs(Eq(static_cast<int>(cp % 997u) + 1)),
                 Field("cluster", &TextGlyph::cluster, Eq(offset)));
  };
  EXPECT_THAT(glyphs, ElementsAre(mapped(0xff21, 0), mapped(0xff11, 1), mapped(0xff01, 2),
                                  mapped(0x30ab, 3), mapped(0x3099, 6), mapped(0xe9, 9),
                                  mapped(0x1f600, 11)));
}

TEST(TextEngineScriptedTest, FullWidthTransformsOnlyPreservedSpaces) {
  for (const char* mode : {"normal", "pre"}) {
    SCOPED_TRACE(mode);
    const auto glyphs =
        LayoutParsedTextPath(std::string("<tspan style='text-transform:full-width;") +
                             "white-space:" + mode + "'>A B</tspan>");
    ASSERT_THAT(glyphs, SizeIs(3));
    const uint32_t space = std::string_view(mode) == "pre" ? 0x3000 : 0x20;
    EXPECT_THAT(glyphs[1], GlyphIndexIs(Eq(static_cast<int>(space % 997u) + 1)));
  }
}

TEST(TextEngineScriptedTest, FullWidthInheritsAndLeavesSourceTextUnchanged) {
  std::vector<ParsedPathRunDetails> details;
  const auto glyphs = LayoutParsedTextPath(
      "<tspan style='text-transform:full-width'>A<tspan style='text-transform:none'>b</tspan>"
      "C</tspan>",
      true, "M0 0H500", false, {}, &details);
  EXPECT_THAT(glyphs, ElementsAre(GlyphIndexIs(Eq(static_cast<int>(0xff21 % 997u) + 1)),
                                  GlyphIndexIs(Eq(static_cast<int>('b') + 1)),
                                  GlyphIndexIs(Eq(static_cast<int>(0xff23 % 997u) + 1))));
  EXPECT_THAT(details, ElementsAre(Field("text", &ParsedPathRunDetails::text, Eq("A")),
                                   Field("text", &ParsedPathRunDetails::text, Eq("b")),
                                   Field("text", &ParsedPathRunDetails::text, Eq("C"))));
}

TEST(TextEngineScriptedTest, FullWidthUsesOriginalCodepointPositions) {
  const auto glyphs = LayoutParsedTextPath(
      "<tspan style='text-transform:full-width' x='10 30 50'>A\xef\xbc\xa2"
      "C</tspan>");
  EXPECT_THAT(glyphs, ElementsAre(GlyphXPositionIs(DoubleEq(12)), GlyphXPositionIs(DoubleEq(32)),
                                  GlyphXPositionIs(DoubleEq(52))));
}

TEST(TextEngineScriptedTest, FullWidthMatchesLiteralUnicodeAcrossAxesAndWhitespace) {
  for (auto mode : {WritingMode::HorizontalTb, WritingMode::VerticalRl, WritingMode::VerticalLr}) {
    for (auto whiteSpace : {WhiteSpace::Pre, WhiteSpace::PreWrap, WhiteSpace::BreakSpaces}) {
      SCOPED_TRACE(testing::Message() << mode << " " << whiteSpace);
      Registry registry;
      FontManager fonts(registry);
      TextEngine engine = MakeScriptedEngine(registry, fonts);
      components::ComputedTextComponent text;
      auto span = MakeSpan("A B\nC\tD");
      span.textTransform = TextTransform::FullWidth;
      span.whiteSpace = whiteSpace;
      span.textAnchor = TextAnchor::End;
      span.wordSpacingPx = 5;
      text.spans.push_back(span);
      auto params = MakeTextParams(20);
      params.writingMode = mode;
      params.inlineSizePx = 31;
      const auto transformed = engine.layout(text, params);
      text.spans[0].textTransform = TextTransform::None;
      text.spans[0].text = RcString(
          "\xef\xbc\xa1\xe3\x80\x80\xef\xbc\xa2\n"
          "\xef\xbc\xa3\t\xef\xbc\xa4");
      text.spans[0].end = text.spans[0].text.size();
      const auto literal = engine.layout(text, params);
      ASSERT_THAT(transformed, SizeIs(1));
      ASSERT_THAT(literal, SizeIs(1));
      ASSERT_THAT(transformed[0].glyphs, SizeIs(literal[0].glyphs.size()));
      const auto& firstB = transformed[0].glyphs[2];
      const double block = isVertical(mode) ? firstB.xPosition : firstB.yPosition;
      const double offset = isVertical(mode) ? 2 : 3;
      const double lineHeight = 1200.0 * static_cast<double>(20.0f / 1200.0f);
      const double line = whiteSpace == WhiteSpace::Pre     ? 0
                          : mode == WritingMode::VerticalRl ? -lineHeight
                                                            : lineHeight;
      EXPECT_THAT(block, DoubleNear(offset + line, 1e-9));
      for (size_t i = 0; i < literal[0].glyphs.size(); ++i) {
        const auto& expected = literal[0].glyphs[i];
        EXPECT_THAT(transformed[0].glyphs[i],
                    AllOf(GlyphIndexIs(Eq(expected.glyphIndex)),
                          GlyphXPositionIs(DoubleNear(expected.xPosition, 1e-9)),
                          GlyphYPositionIs(DoubleNear(expected.yPosition, 1e-9)),
                          Field("sideways", &TextGlyph::sideways, Eq(expected.sideways))));
        EXPECT_THAT(transformed[0].glyphs[i].cluster, Eq(i));
      }
    }
  }
}

TEST(TextEngineScriptedTest, FullWidthPathUsesMappedTextAndSourceClusters) {
  const auto transformed = LayoutParsedTextPath(
      "<textPath href='#p' style='text-transform:full-width;white-space:pre;word-spacing:5px'>"
      "A B</textPath>");
  const auto literal = LayoutParsedTextPath(
      "<textPath href='#p' style='white-space:pre;word-spacing:5px'>"
      "\xef\xbc\xa1\xe3\x80\x80\xef\xbc\xa2</textPath>");
  ASSERT_THAT(transformed, SizeIs(3));
  ASSERT_THAT(literal, SizeIs(3));
  for (size_t i = 0; i < literal.size(); ++i) {
    EXPECT_THAT(transformed[i], AllOf(GlyphIndexIs(Eq(literal[i].glyphIndex)),
                                      GlyphXPositionIs(DoubleNear(literal[i].xPosition, 1e-9)),
                                      Field("cluster", &TextGlyph::cluster, Eq(i))));
  }
}

TEST(TextEngineScriptedTest, FullWidthRestoresReversedVisualSourceClusters) {
  Registry registry;
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts, true);
  components::ComputedTextComponent text;
  auto span = MakeSpan(
      "A\xc3\xa9"
      "B");
  span.textTransform = TextTransform::FullWidth;
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(1));
  EXPECT_THAT(runs[0].glyphs, ElementsAre(Field("cluster", &TextGlyph::cluster, Eq(3u)),
                                          Field("cluster", &TextGlyph::cluster, Eq(1u)),
                                          Field("cluster", &TextGlyph::cluster, Eq(0u))));
}

TEST(TextEngineScriptedTest, FullWidthPreservedLinesKeepVisualOrderAndOriginalIndices) {
  class LineBoundaryBackend : public ScriptedTextBackend {
  public:
    mutable size_t boundaryKernCalls = 0;
    double crossSpanKern(FontHandle, float, FontHandle, float, uint32_t, uint32_t,
                         bool vertical) const override {
      ++boundaryKernCalls;
      return vertical ? 4.0 : 3.0;
    }
  };
  Registry registry;
  FontManager fonts(registry);
  auto backend = std::make_unique<LineBoundaryBackend>();
  backend->reverseClusters = true;
  const auto& probe = *backend;
  TextEngine engine(fonts, registry, std::move(backend));
  components::ComputedTextComponent text;
  auto span = MakeSpan("AB\nCD");
  span.textTransform = TextTransform::FullWidth;
  span.whiteSpace = WhiteSpace::Pre;
  text.spans.push_back(span);
  const auto runs = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(runs, SizeIs(1));
  ASSERT_THAT(runs[0].glyphs, SizeIs(5));
  EXPECT_THAT(runs[0].glyphs,
              ElementsAre(Field(&TextGlyph::cluster, Eq(1u)), Field(&TextGlyph::cluster, Eq(0u)),
                          Field(&TextGlyph::cluster, Eq(2u)), Field(&TextGlyph::cluster, Eq(4u)),
                          Field(&TextGlyph::cluster, Eq(3u))));
  EXPECT_THAT(runs[0].glyphs[3].yPosition - runs[0].glyphs[0].yPosition, DoubleNear(20.0, 1e-5));
  EXPECT_THAT(probe.boundaryKernCalls, Eq(0u));
}

TEST(TextEngineScriptedTest, FullWidthUsesCssWithLegacySpaceAndIgnoresNonPresentationAttribute) {
  const auto preserved = LayoutParsedTextPath(
      "<tspan xml:space='preserve' style='text-transform:full-width'>A B</tspan>");
  ASSERT_THAT(preserved, SizeIs(3));
  EXPECT_THAT(preserved[1], GlyphIndexIs(Eq(static_cast<int>(0x3000 % 997u) + 1)));
  const auto attributeOnly = LayoutParsedTextPath("<tspan text-transform='full-width'>A</tspan>");
  EXPECT_THAT(attributeOnly, ElementsAre(GlyphIndexIs(Eq(static_cast<int>('A') + 1))));
  const auto overridden = LayoutParsedTextPath(
      "<tspan text-transform='full-width' style='text-transform:none'>A</tspan>");
  EXPECT_THAT(overridden, ElementsAre(GlyphIndexIs(Eq(static_cast<int>('A') + 1))));
}

TEST(TextEngineScriptedTest, FullWidthCharacterApisRetainOriginalUtf16Indices) {
  const std::string sourceText =
      "A\xc3\xa9\xf0\x9f\x98\x80"
      "B";
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><text id='t' font-size='20' "
      "style='text-transform:full-width'>" +
          sourceText + "</text></svg>",
      warnings);
  ASSERT_THAT(parsed, ParseResultIs(_));
  auto document = std::move(parsed).result();
  Registry& registry = document.registry();
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  const auto target = document.querySelector("#t");
  ASSERT_THAT(target, testing::Optional(_));
  const auto handle = target->unsafeEntityHandle();
  engine.prepareForElement(handle, warnings);
  EXPECT_THAT(engine.getNumberOfChars(handle), Eq(5));
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 4).x, DoubleNear(35, 1e-9));
  EXPECT_THAT(engine.getStartPositionOfChar(handle, 2).x,
              DoubleNear(engine.getStartPositionOfChar(handle, 3).x, 1e-9));
  EXPECT_THAT(handle.get<components::TextComponent>().text.str(), Eq(sourceText));
}

TEST(TextEngineScriptedTest, FullWidthWordSpacingExcludesIdeographicSpacesOnFlatAndPathText) {
  struct SpaceCase {
    const char* content;
    const char* style;
    double addedSpacing;
  };
  const SpaceCase cases[] = {
      {"A\xe3\x80\x80"
       "B",
       "white-space:pre", 0},
      {"A B", "white-space:pre;text-transform:full-width", 0},
      {"A B", "white-space:normal;text-transform:full-width", 5},
  };
  for (const auto& space : cases) {
    for (bool onPath : {false, true}) {
      SCOPED_TRACE(testing::Message() << space.style << " path=" << onPath);
      std::array<double, 2> positions{};
      for (size_t index = 0; index < positions.size(); ++index) {
        const std::string tag = onPath ? "textPath" : "tspan";
        const std::string attributes = onPath ? " href='#p'" : "";
        const auto glyphs = LayoutParsedTextPath("<" + tag + attributes + " style='" + space.style +
                                                 ";word-spacing:" + std::to_string(index * 5) +
                                                 "px'>" + space.content + "</" + tag + ">");
        ASSERT_THAT(glyphs, SizeIs(3));
        positions[index] = glyphs.back().xPosition;
      }
      EXPECT_THAT(positions[1] - positions[0], DoubleNear(space.addedSpacing, 1e-9));
    }
  }
}

TEST(TextEngineScriptedTest, FullWidthStyleMutationInvalidatesGeometryAndPreservesSource) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><text id='t'>A</text></svg>", warnings);
  ASSERT_THAT(parsed, ParseResultIs(_));
  auto document = std::move(parsed).result();
  auto target = document.querySelector("#t");
  ASSERT_THAT(target, testing::Optional(_));
  const auto handle = target->unsafeEntityHandle();
  Registry& registry = document.registry();
  FontManager fonts(registry);
  TextEngine engine = MakeScriptedEngine(registry, fonts);
  for (TextTransform transform :
       {TextTransform::None, TextTransform::FullWidth, TextTransform::None}) {
    SCOPED_TRACE(transform);
    target->setStyle(transform == TextTransform::FullWidth ? "text-transform:full-width"
                                                           : "text-transform:none");
    engine.prepareForElement(handle, warnings);
    const auto& geometry = engine.ensureComputedTextGeometryComponent(handle);
    const uint32_t codepoint = transform == TextTransform::FullWidth ? 0xff21 : 'A';
    EXPECT_THAT(geometry.runs, ElementsAre(RunGlyphsAre(ElementsAre(
                                   GlyphIndexIs(Eq(static_cast<int>(codepoint % 997u) + 1))))));
    EXPECT_THAT(engine.getNumberOfChars(handle), Eq(1));
    EXPECT_THAT(handle.get<components::TextComponent>().text.str(), Eq("A"));
  }
}

TEST(TextEngineScriptedTest, FullWidthFallbackUsesTransformedCodepointAndOriginalCluster) {
  Registry registry;
  FontManager fonts(registry);
  const FontHandle coveringFont = RegisterFallbackBackedFace(fonts, "WidthFace");
  ASSERT_THAT(coveringFont, Not(Eq(FontHandle{})));
  auto backend = std::make_unique<CoverageProbeBackend>();
  backend->coveringFont = coveringFont;
  TextEngine engine(fonts, registry, std::move(backend));
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A"));
  const auto original = engine.layout(text, MakeTextParams(20));
  ASSERT_THAT(original, ElementsAre(RunGlyphsAre(SizeIs(1))));
  EXPECT_THAT(original[0].glyphs[0].font, Not(Eq(coveringFont)));
  text.spans[0].textTransform = TextTransform::FullWidth;
  const auto transformed = engine.layout(text, MakeTextParams(20));
  EXPECT_THAT(transformed, ElementsAre(RunGlyphsAre(ElementsAre(
                               AllOf(GlyphIndexIs(Eq(static_cast<int>(0xff21 % 997u) + 1)),
                                     Field("font", &TextGlyph::font, Eq(coveringFont)),
                                     Field("cluster", &TextGlyph::cluster, Eq(0u)))))));
}

}  // namespace donner::svg
