#include "donner/svg/renderer/RendererDriver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/Specificity.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Eq;
using ::testing::Field;

namespace donner::svg {
namespace {

MATCHER_P(Vector2dEq, expected, "matches Vector2d") {
  return arg.x == expected.x && arg.y == expected.y;
}

MATCHER(IsIdentityTransform, "identity transform") {
  const Transform2d identity;
  for (size_t i = 0; i < 6; ++i) {
    if (std::abs(arg.data[i] - identity.data[i]) > 1e-9) {
      return false;
    }
  }
  return true;
}

MATCHER_P(PathCommandsAre, expected, "path commands match") {
  const auto& commands = arg.path.commands();
  if (commands.size() != expected.size()) {
    return false;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    if (commands[i].verb != expected[i]) {
      return false;
    }
  }
  return arg.fillRule == FillRule::NonZero;
}

MATCHER_P(ClipHasPaths, count, "clip has expected path count") {
  const bool hasExpectedCount = arg.clipPaths.size() == count;
  const bool hasNoMask = !arg.mask.has_value();
  const bool hasNoRect = !arg.clipRect.has_value();
  return hasExpectedCount && hasNoMask && hasNoRect;
}

MATCHER(HasClipRect, "clip contains a rectangle") {
  return arg.clipRect.has_value();
}

MATCHER_P(BoxSizeEq, expected, "box has expected size") {
  return arg.size() == expected;
}

MATCHER_P2(TransformNear, expected, tolerance, "transform near") {
  for (size_t i = 0; i < 6; ++i) {
    if (std::abs(arg.data[i] - expected.data[i]) > tolerance) {
      return false;
    }
  }
  return true;
}

class MockRendererInterface : public RendererInterface {
public:
  MOCK_METHOD(void, draw, (SVGDocument & document), (override));
  MOCK_METHOD(int, width, (), (const, override));
  MOCK_METHOD(int, height, (), (const, override));
  MOCK_METHOD(void, beginFrame, (const RenderViewport& viewport), (override));
  MOCK_METHOD(void, endFrame, (), (override));
  MOCK_METHOD(void, setTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, pushTransform, (const Transform2d& transform), (override));
  MOCK_METHOD(void, popTransform, (), (override));
  MOCK_METHOD(void, pushClip, (const ResolvedClip& clip), (override));
  MOCK_METHOD(void, popClip, (), (override));
  MOCK_METHOD(void, pushIsolatedLayer, (double opacity, MixBlendMode blendMode), (override));
  MOCK_METHOD(void, popIsolatedLayer, (), (override));
  MOCK_METHOD(void, pushFilterLayer,
              (const components::FilterGraph& filterGraph,
               const std::optional<Box2d>& filterRegion),
              (override));
  MOCK_METHOD(void, popFilterLayer, (), (override));
  MOCK_METHOD(void, pushMask, (const std::optional<Box2d>& maskBounds), (override));
  MOCK_METHOD(void, transitionMaskToContent, (), (override));
  MOCK_METHOD(void, popMask, (), (override));
  MOCK_METHOD(void, beginPatternTile, (const Box2d& tileRect, const Transform2d& targetFromPattern),
              (override));
  MOCK_METHOD(void, endPatternTile, (bool forStroke), (override));
  MOCK_METHOD(void, setPaint, (const PaintParams& paint), (override));
  MOCK_METHOD(void, drawPath, (const PathShape& path, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawRect, (const Box2d& rect, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawEllipse, (const Box2d& bounds, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawImage, (const ImageResource& image, const ImageParams& params), (override));
  MOCK_METHOD(void, drawText,
              (Registry & registry, const components::ComputedTextComponent& text,
               const TextParams& params),
              (override));
  MOCK_METHOD(RendererBitmap, takeSnapshot, (), (const, override));
  MOCK_METHOD(std::unique_ptr<RendererInterface>, createOffscreenInstance, (), (const, override));
};

class RendererDriverTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  ::testing::NiceMock<MockRendererInterface> renderer;
  RendererDriver driver{renderer};
};

TEST_F(RendererDriverTest, BeginsAndEndsFrameAroundTraversal) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="8" height="6" fill="red" />
  )svg");

  RendererBitmap snapshot;
  snapshot.dimensions = Vector2i(16, 16);
  snapshot.rowBytes = snapshot.dimensions.x * 4;
  snapshot.pixels.resize(snapshot.rowBytes * snapshot.dimensions.y, 255);

  const std::array<Path::Verb, 5> rectCommands = {Path::Verb::MoveTo, Path::Verb::LineTo,
                                                  Path::Verb::LineTo, Path::Verb::LineTo,
                                                  Path::Verb::ClosePath};

  const auto viewportSizeMatcher = Field(&RenderViewport::size, Vector2dEq(Vector2d(16, 16)));

  EXPECT_CALL(renderer, beginFrame(viewportSizeMatcher)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(PathCommandsAre(rectCommands), _)).Times(AtLeast(1));

  EXPECT_CALL(renderer, takeSnapshot()).WillOnce(testing::Return(snapshot));

  driver.draw(document);

  const RendererBitmap bitmap = driver.takeSnapshot();
  EXPECT_FALSE(bitmap.empty());
  EXPECT_THAT(bitmap.dimensions, Eq(Vector2i(16, 16)));
}

TEST_F(RendererDriverTest, EmitsClipPathsWhenPresent) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <clipPath id="clip">
        <rect x="1" y="2" width="4" height="4" />
      </clipPath>
    </defs>
    <g clip-path="url(#clip)">
      <rect x="0" y="0" width="6" height="6" />
    </g>
  )svg",
                                      Vector2i(8, 8));

  const bool hasClipComponents =
      !document.registry().view<components::ComputedClipPathsComponent>().empty();

  int clipPushCount = 0;
  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushClip(_))
      .Times(::testing::AnyNumber())
      .WillRepeatedly([&](const ResolvedClip& clip) {
        if (!clip.empty()) {
          ++clipPushCount;
        }
      });
  EXPECT_CALL(renderer, popClip()).Times(::testing::AnyNumber());

  driver.draw(document);
  if (hasClipComponents) {
    EXPECT_GE(clipPushCount, 1);
  }
}

TEST_F(RendererDriverTest, EmitsTextDrawCallsForSolidFill) {
  SVGDocument document = makeDocument(R"svg(
    <text x="2" y="3" fill="#00FF00">hi</text>
  )svg",
                                      Vector2i(12, 12));

  EXPECT_CALL(renderer, beginFrame(_)).WillOnce([&](const RenderViewport&) {
    Entity textEntity = document.registry().create();
    auto& textInstance =
        document.registry().emplace<components::RenderingInstanceComponent>(textEntity);
    textInstance.dataEntity = textEntity;
    textInstance.entityFromWorldTransform = Transform2d();
    textInstance.resolvedFill = PaintServer::Solid(css::Color(css::RGBA::RGB(0, 255, 0)));

    const Vector2i canvasSize = document.canvasSize();
    document.registry().emplace<components::ComputedViewBoxComponent>(
        textEntity, Box2d(Vector2d(), Vector2d(static_cast<double>(canvasSize.x),
                                               static_cast<double>(canvasSize.y))));

    components::ComputedStyleComponent style;
    PropertyRegistry properties;
    const css::Specificity inlineSpecificity = css::Specificity::FromABC(1, 0, 0);
    properties.opacity.set(1.0, inlineSpecificity);
    properties.color.set(css::Color(css::RGBA(0, 0, 0, 255)), inlineSpecificity);
    properties.strokeOpacity.set(1.0, inlineSpecificity);
    properties.fontFamily.set(SmallVector<RcString, 1>{RcString("sans-serif")}, inlineSpecificity);
    properties.fontSize.set(Lengthd(12, Lengthd::Unit::None), inlineSpecificity);
    style.properties = properties;
    document.registry().emplace<components::ComputedStyleComponent>(textEntity, style);

    components::ComputedTextComponent text;
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("hi");
    span.start = 0;
    span.end = 2;
    span.xList.push_back(Lengthd(2, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(3, Lengthd::Unit::None));
    span.startsNewChunk = true;
    text.spans.push_back(std::move(span));
    document.registry().emplace<components::ComputedTextComponent>(textEntity, text);
  });
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(
      renderer,
      drawText(_, _, Field(&TextParams::fillColor, Eq(css::Color(css::RGBA(0, 255, 0, 255))))))
      .Times(AtLeast(1));

  driver.draw(document);
}

TEST_F(RendererDriverTest, ResolvesSpanStrokeAndDecorationPaintFromTextStyle) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="1" height="1" fill="none" />
  )svg",
                                      Vector2i(200, 200));

  auto expectSolidColor = [](const components::ResolvedPaintServer& paint,
                             const css::RGBA& expected) {
    const auto* solid = std::get_if<PaintServer::Solid>(&paint);
    ASSERT_NE(solid, nullptr);
    EXPECT_EQ(solid->color.resolve(css::RGBA(0, 0, 0, 255), 1.0f), expected);
  };

  EXPECT_CALL(renderer, beginFrame(_)).WillOnce([&](const RenderViewport&) {
    const css::Specificity inlineSpecificity = css::Specificity::FromABC(1, 0, 0);
    const auto setCommonTextProps = [&](PropertyRegistry& properties) {
      properties.opacity.set(1.0, inlineSpecificity);
      properties.color.set(css::Color(css::RGBA(0, 0, 0, 255)), inlineSpecificity);
      properties.fillOpacity.set(1.0, inlineSpecificity);
      properties.strokeOpacity.set(1.0, inlineSpecificity);
      properties.strokeWidth.set(Lengthd(1, Lengthd::Unit::None), inlineSpecificity);
      properties.fontFamily.set(SmallVector<RcString, 1>{RcString("sans-serif")},
                                inlineSpecificity);
      properties.fontSize.set(Lengthd(48, Lengthd::Unit::None), inlineSpecificity);
    };

    const Vector2i canvasSize = document.canvasSize();
    const Box2d viewBox = Box2d(
        Vector2d(), Vector2d(static_cast<double>(canvasSize.x), static_cast<double>(canvasSize.y)));

    Entity groupEntity = document.registry().create();
    Entity textEntity = document.registry().create();
    Entity tspanEntity = document.registry().create();

    document.registry().emplace<::donner::components::TreeComponent>(groupEntity,
                                                                     xml::XMLQualifiedNameRef("g"));
    document.registry().emplace<::donner::components::TreeComponent>(
        textEntity, xml::XMLQualifiedNameRef("text"));
    document.registry().emplace<::donner::components::TreeComponent>(
        tspanEntity, xml::XMLQualifiedNameRef("tspan"));
    document.registry()
        .get<::donner::components::TreeComponent>(groupEntity)
        .appendChild(document.registry(), textEntity);
    document.registry()
        .get<::donner::components::TreeComponent>(textEntity)
        .appendChild(document.registry(), tspanEntity);

    document.registry().emplace<components::TextComponent>(textEntity);
    document.registry().emplace<components::TextComponent>(tspanEntity);

    auto& textInstance =
        document.registry().emplace<components::RenderingInstanceComponent>(textEntity);
    textInstance.dataEntity = textEntity;
    textInstance.entityFromWorldTransform = Transform2d();
    textInstance.resolvedFill = PaintServer::Solid(css::Color(css::RGBA(255, 255, 0, 255)));
    textInstance.resolvedStroke = PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 255)));

    document.registry().emplace<components::ComputedViewBoxComponent>(textEntity, viewBox);

    components::ComputedStyleComponent groupStyle;
    setCommonTextProps(groupStyle.properties.emplace());
    groupStyle.properties->fill.set(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255))),
                                    inlineSpecificity);
    groupStyle.properties->stroke.set(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255))),
                                      inlineSpecificity);
    groupStyle.properties->textDecoration.set(TextDecoration::LineThrough, inlineSpecificity);
    document.registry().emplace<components::ComputedStyleComponent>(groupEntity, groupStyle);

    components::ComputedStyleComponent textStyle;
    setCommonTextProps(textStyle.properties.emplace());
    textStyle.properties->fill.set(PaintServer::Solid(css::Color(css::RGBA(255, 255, 0, 255))),
                                   inlineSpecificity);
    textStyle.properties->stroke.set(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 255))),
                                     inlineSpecificity);
    document.registry().emplace<components::ComputedStyleComponent>(textEntity, textStyle);

    components::ComputedStyleComponent tspanStyle;
    setCommonTextProps(tspanStyle.properties.emplace());
    tspanStyle.properties->fill.set(PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255))),
                                    inlineSpecificity);
    tspanStyle.properties->stroke.set(PaintServer::Solid(css::Color(css::RGBA(128, 128, 128, 255))),
                                      inlineSpecificity);
    document.registry().emplace<components::ComputedStyleComponent>(tspanEntity, tspanStyle);

    components::ComputedTextComponent text;
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("Text");
    span.start = 0;
    span.end = 4;
    span.sourceEntity = tspanEntity;
    span.xList.push_back(Lengthd(50, Lengthd::Unit::None));
    span.yList.push_back(Lengthd(100, Lengthd::Unit::None));
    span.startsNewChunk = true;
    text.spans.push_back(std::move(span));
    document.registry().emplace<components::ComputedTextComponent>(textEntity, std::move(text));
  });
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawText(_, _, _))
      .WillOnce([&](Registry&, const components::ComputedTextComponent& text, const TextParams&) {
        const auto spanIt =
            std::find_if(text.spans.begin(), text.spans.end(), [](const auto& span) {
              return std::string_view(span.text).substr(span.start, span.end - span.start) ==
                     "Text";
            });

        ASSERT_NE(spanIt, text.spans.end());
        EXPECT_EQ(spanIt->textDecoration, TextDecoration::LineThrough);
        expectSolidColor(spanIt->resolvedStroke, css::RGBA(128, 128, 128, 255));
        expectSolidColor(spanIt->resolvedDecorationFill, css::RGBA(255, 255, 0, 255));
        expectSolidColor(spanIt->resolvedDecorationStroke, css::RGBA(0, 128, 0, 255));
      });

  driver.draw(document);
}

TEST_F(RendererDriverTest, EmitsImageDrawCallsWithClipAndTransform) {
  SVGDocument document = makeDocument(R"svg(
    <image x="1" y="1" width="4" height="2" href="dummy.png" />
  )svg",
                                      Vector2i(10, 8));

  auto images = document.registry().view<components::ImageComponent>();
  ASSERT_FALSE(images.empty());

  for (const Entity entity : images) {
    auto& loaded = document.registry().emplace<components::LoadedImageComponent>(entity);
    loaded.image = ImageResource{};
    loaded.image->width = 2;
    loaded.image->height = 3;
    loaded.image->data = std::vector<uint8_t>(
        static_cast<size_t>(loaded.image->width * loaded.image->height * 4), 255);
  }

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, popTransform()).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushClip(HasClipRect())).Times(AtLeast(1));

  EXPECT_CALL(renderer, drawImage(_, Field(&ImageParams::targetRect, BoxSizeEq(Vector2d(2, 3)))))
      .Times(AtLeast(1));

  driver.draw(document);
}

TEST_F(RendererDriverTest, AppliesDefaultPreserveAspectRatioWhenComponentMissing) {
  SVGDocument document = makeDocument(R"svg(
    <image x="2" y="0" width="6" height="4" href="dummy.png" />
  )svg",
                                      Vector2i(12, 6));

  auto images = document.registry().view<components::ImageComponent>();
  ASSERT_FALSE(images.empty());

  std::vector<Transform2d> transforms;
  EXPECT_CALL(renderer, pushTransform(_))
      .Times(testing::AtLeast(1))
      .WillRepeatedly([&](const Transform2d& transform) { transforms.push_back(transform); });

  const Box2d bounds = Box2d::FromXYWH(2, 0, 6, 4);

  for (const Entity entity : images) {
    document.registry().remove<components::PreserveAspectRatioComponent>(entity);

    auto& loaded = document.registry().emplace<components::LoadedImageComponent>(entity);
    loaded.image = ImageResource{};
    loaded.image->width = 3;
    loaded.image->height = 2;
    loaded.image->data = std::vector<uint8_t>(
        static_cast<size_t>(loaded.image->width * loaded.image->height * 4), 255);

    const Box2d intrinsicSize =
        Box2d::WithSize(Vector2d(loaded.image->width, loaded.image->height));
    const Transform2d expectedTransform =
        PreserveAspectRatio::Default().elementContentFromViewBoxTransform(bounds, intrinsicSize);
    EXPECT_THAT(transforms,
                testing::Not(testing::Contains(TransformNear(expectedTransform, 1e-6))));
  }

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushClip(HasClipRect())).Times(AtLeast(1));
  EXPECT_CALL(renderer, popClip()).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawImage(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, popTransform()).Times(AtLeast(1));

  driver.draw(document);

  const Box2d intrinsicSize = Box2d::WithSize(Vector2d(3, 2));
  const Transform2d expectedTransform =
      PreserveAspectRatio::Default().elementContentFromViewBoxTransform(bounds, intrinsicSize);
  EXPECT_THAT(transforms, testing::Contains(TransformNear(expectedTransform, 1e-6)));
}

TEST_F(RendererDriverTest, EmitsMaskSequenceForMaskedElement) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <mask id="m">
        <rect x="0" y="0" width="8" height="8" fill="white" />
      </mask>
    </defs>
    <rect x="0" y="0" width="8" height="8" fill="blue" mask="url(#m)" />
  )svg",
                                      Vector2i(8, 8));

  // Track the call sequence for mask operations.
  int pushMaskCount = 0;
  int transitionCount = 0;
  int popMaskCount = 0;
  int drawPathAfterTransition = 0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));

  EXPECT_CALL(renderer, pushMask(_)).WillRepeatedly([&](const std::optional<Box2d>&) {
    ++pushMaskCount;
  });
  EXPECT_CALL(renderer, transitionMaskToContent()).WillRepeatedly([&]() {
    EXPECT_GE(pushMaskCount, 1) << "transitionMaskToContent called before pushMask";
    ++transitionCount;
  });
  EXPECT_CALL(renderer, popMask()).WillRepeatedly([&]() {
    EXPECT_GE(transitionCount, 1) << "popMask called before transitionMaskToContent";
    ++popMaskCount;
  });

  // The mask content and the masked element both emit drawPath calls.
  EXPECT_CALL(renderer, drawPath(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](const PathShape&, const StrokeParams&) {
        if (transitionCount > 0) {
          ++drawPathAfterTransition;
        }
      });

  driver.draw(document);

  EXPECT_GE(pushMaskCount, 1) << "pushMask should be called at least once";
  EXPECT_GE(transitionCount, 1) << "transitionMaskToContent should be called at least once";
  EXPECT_GE(popMaskCount, 1) << "popMask should be called at least once";
  EXPECT_EQ(pushMaskCount, popMaskCount) << "pushMask and popMask should be paired";
  EXPECT_GE(drawPathAfterTransition, 1)
      << "The masked element should be drawn after transitionMaskToContent";
}

TEST_F(RendererDriverTest, EmitsIsolatedLayerForOpacity) {
  SVGDocument document = makeDocument(R"svg(
    <g opacity="0.5">
      <rect x="0" y="0" width="6" height="6" fill="red" />
    </g>
  )svg",
                                      Vector2i(8, 8));

  int pushLayerCount = 0;
  int popLayerCount = 0;
  double capturedOpacity = -1.0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  EXPECT_CALL(renderer, pushIsolatedLayer(_, _))
      .WillRepeatedly([&](double opacity, MixBlendMode blendMode) {
        ++pushLayerCount;
        capturedOpacity = opacity;
        EXPECT_EQ(blendMode, MixBlendMode::Normal) << "Default blend mode should be Normal";
      });
  EXPECT_CALL(renderer, popIsolatedLayer()).WillRepeatedly([&]() { ++popLayerCount; });

  driver.draw(document);

  EXPECT_GE(pushLayerCount, 1) << "pushIsolatedLayer should be called for opacity < 1.0";
  EXPECT_EQ(pushLayerCount, popLayerCount) << "push/pop should be paired";
  EXPECT_NEAR(capturedOpacity, 0.5, 1e-9) << "Opacity should be 0.5";
}

TEST_F(RendererDriverTest, EmitsIsolatedLayerForMixBlendMode) {
  SVGDocument document = makeDocument(R"svg(
    <g style="mix-blend-mode: multiply">
      <rect x="0" y="0" width="4" height="4" fill="green" />
    </g>
  )svg",
                                      Vector2i(8, 8));

  bool foundMultiplyBlend = false;
  int pushLayerCount = 0;
  int popLayerCount = 0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  EXPECT_CALL(renderer, pushIsolatedLayer(_, _))
      .WillRepeatedly([&](double opacity, MixBlendMode blendMode) {
        ++pushLayerCount;
        if (blendMode == MixBlendMode::Multiply) {
          foundMultiplyBlend = true;
          EXPECT_NEAR(opacity, 1.0, 1e-9) << "Opacity should be 1.0 when only blend mode is set";
        }
      });
  EXPECT_CALL(renderer, popIsolatedLayer()).WillRepeatedly([&]() { ++popLayerCount; });

  driver.draw(document);

  EXPECT_TRUE(foundMultiplyBlend)
      << "A pushIsolatedLayer call with Multiply blend mode is expected";
  EXPECT_EQ(pushLayerCount, popLayerCount) << "push/pop should be paired";
}

TEST_F(RendererDriverTest, EmitsPatternTileForPatternFill) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <pattern id="pat" x="0" y="0" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect x="0" y="0" width="2" height="2" fill="red" />
      </pattern>
    </defs>
    <rect x="0" y="0" width="8" height="8" fill="url(#pat)" />
  )svg",
                                      Vector2i(8, 8));

  int beginPatternCount = 0;
  int endPatternCount = 0;
  int drawPathInPattern = 0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));

  EXPECT_CALL(renderer, beginPatternTile(_, _))
      .WillRepeatedly([&](const Box2d& tileRect, const Transform2d&) {
        ++beginPatternCount;
        // Pattern tile should have the specified 4x4 size.
        EXPECT_NEAR(tileRect.width(), 4.0, 1e-6);
        EXPECT_NEAR(tileRect.height(), 4.0, 1e-6);
      });
  EXPECT_CALL(renderer, endPatternTile(_)).WillRepeatedly([&](bool) { ++endPatternCount; });

  EXPECT_CALL(renderer, drawPath(_, _))
      .Times(AtLeast(1))
      .WillRepeatedly([&](const PathShape&, const StrokeParams&) {
        if (beginPatternCount > 0 && endPatternCount == 0) {
          ++drawPathInPattern;
        }
      });

  driver.draw(document);

  EXPECT_GE(beginPatternCount, 1) << "beginPatternTile should be called for pattern fill";
  EXPECT_EQ(beginPatternCount, endPatternCount) << "begin/end pattern tile should be paired";
  EXPECT_GE(drawPathInPattern, 1) << "Pattern content should be drawn between begin and end";
}

TEST_F(RendererDriverTest, DrawsEllipseAndRectAsPath) {
  SVGDocument document = makeDocument(R"svg(
    <ellipse cx="6" cy="6" rx="4" ry="3" fill="blue" />
    <rect x="0" y="0" width="5" height="3" fill="red" />
  )svg",
                                      Vector2i(12, 12));

  int drawPathCount = 0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));

  // The RendererDriver converts all shapes (ellipse, rect, etc.) to paths via
  // ComputedPathComponent, so drawPath should be called for each visible shape element.
  EXPECT_CALL(renderer, drawPath(_, _)).WillRepeatedly([&](const PathShape&, const StrokeParams&) {
    ++drawPathCount;
  });

  // drawRect and drawEllipse are backend-level optimizations, not used by RendererDriver.
  EXPECT_CALL(renderer, drawRect(_, _)).Times(0);
  EXPECT_CALL(renderer, drawEllipse(_, _)).Times(0);

  driver.draw(document);

  EXPECT_GE(drawPathCount, 2) << "Both the ellipse and the rect should be drawn via drawPath";
}

TEST_F(RendererDriverTest, AccumulatesTransformsForNestedGroups) {
  SVGDocument document = makeDocument(R"svg(
    <g transform="translate(10, 0)">
      <g transform="translate(0, 20)">
        <rect x="0" y="0" width="2" height="2" fill="red" />
      </g>
    </g>
  )svg",
                                      Vector2i(40, 40));

  std::vector<Transform2d> setTransformCalls;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  EXPECT_CALL(renderer, setTransform(_))
      .Times(AtLeast(1))
      .WillRepeatedly(
          [&](const Transform2d& transform) { setTransformCalls.push_back(transform); });

  driver.draw(document);

  // The inner rect should receive a combined translate(10, 20) transform.
  // The driver computes absolute transforms during preparation and sets them via setTransform.
  const Transform2d expectedCombined = Transform2d::Translate(Vector2d(10, 20));
  EXPECT_THAT(setTransformCalls, testing::Contains(TransformNear(expectedCombined, 1e-6)))
      << "setTransform should be called with the combined translate(10, 20) transform";

  // Also verify that the outer group's translate(10, 0) transform appears separately.
  const Transform2d outerTransform = Transform2d::Translate(Vector2d(10, 0));
  EXPECT_THAT(setTransformCalls, testing::Contains(TransformNear(outerTransform, 1e-6)))
      << "setTransform should be called with translate(10, 0) for the outer group";
}

TEST_F(RendererDriverTest, EmitsIsolatedLayerForOpacityWithBlendMode) {
  SVGDocument document = makeDocument(R"svg(
    <g opacity="0.7" style="mix-blend-mode: screen">
      <rect x="0" y="0" width="6" height="6" fill="blue" />
    </g>
  )svg",
                                      Vector2i(8, 8));

  bool foundScreenWithOpacity = false;
  int pushLayerCount = 0;
  int popLayerCount = 0;

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));

  EXPECT_CALL(renderer, pushIsolatedLayer(_, _))
      .WillRepeatedly([&](double opacity, MixBlendMode blendMode) {
        ++pushLayerCount;
        if (blendMode == MixBlendMode::Screen) {
          foundScreenWithOpacity = true;
          EXPECT_NEAR(opacity, 0.7, 1e-9);
        }
      });
  EXPECT_CALL(renderer, popIsolatedLayer()).WillRepeatedly([&]() { ++popLayerCount; });

  driver.draw(document);

  EXPECT_TRUE(foundScreenWithOpacity)
      << "pushIsolatedLayer should be called with Screen blend mode and opacity 0.7";
  EXPECT_EQ(pushLayerCount, popLayerCount) << "push/pop should be paired";
}

TEST_F(RendererDriverTest, DrawEntityRangeDefersSubtreeCleanupAndAppliesBaseTransform) {
  SVGDocument document = makeDocument(R"svg(
    <g opacity="0.5">
      <rect x="1" y="2" width="6" height="4" fill="red" />
    </g>
  )svg",
                                      Vector2i(20, 20));

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  ASSERT_FALSE(warnings.hasWarnings());

  Entity subtreeRoot = entt::null;
  Entity subtreeLast = entt::null;
  for (auto view = document.registry().view<components::RenderingInstanceComponent>();
       const Entity entity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (instance.subtreeInfo.has_value()) {
      subtreeRoot = entity;
      subtreeLast = instance.subtreeInfo->lastRenderedEntity;
      break;
    }
  }

  ASSERT_TRUE(subtreeRoot != entt::null);
  ASSERT_TRUE(subtreeLast != entt::null);

  std::vector<Transform2d> transforms;
  int pushLayerCount = 0;
  int popLayerCount = 0;
  const Transform2d baseTransform = Transform2d::Translate(Vector2d(5, 7));

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_))
      .Times(AtLeast(1))
      .WillRepeatedly([&](const Transform2d& transform) { transforms.push_back(transform); });
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushIsolatedLayer(0.5, MixBlendMode::Normal))
      .WillOnce([&](double, MixBlendMode) { ++pushLayerCount; });
  EXPECT_CALL(renderer, popIsolatedLayer()).WillOnce([&]() { ++popLayerCount; });

  RenderViewport viewport;
  viewport.size = Vector2d(20, 20);
  viewport.devicePixelRatio = 1.0;

  driver.drawEntityRange(document.registry(), subtreeRoot, subtreeLast, viewport, baseTransform);

  EXPECT_EQ(pushLayerCount, 1);
  EXPECT_EQ(popLayerCount, 1);
  EXPECT_THAT(transforms, testing::Contains(TransformNear(baseTransform, 1e-6)));
}

TEST_F(RendererDriverTest, DrawEntityRangeConvertsCssFilterFunctionsToFilterGraph) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="10" y="20" width="40" height="10" fill="red" />
  )svg",
                                      Vector2i(200, 100));

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  ASSERT_FALSE(warnings.hasWarnings());

  Entity rectEntity = entt::null;
  for (auto view = document.registry().view<components::RenderingInstanceComponent>();
       const Entity entity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (instance.dataHandle(document.registry()).all_of<components::ComputedPathComponent>()) {
      rectEntity = entity;
      break;
    }
  }

  ASSERT_TRUE(rectEntity != entt::null);

  auto& instance = document.registry().get<components::RenderingInstanceComponent>(rectEntity);
  auto& style = document.registry().get<components::ComputedStyleComponent>(rectEntity);
  ASSERT_TRUE(style.properties.has_value());

  const css::Specificity inlineSpecificity = css::Specificity::FromABC(1, 0, 0);
  style.properties->color.set(css::Color(css::RGBA(12, 34, 56, 255)), inlineSpecificity);

  FilterEffect::DropShadow shadow;
  shadow.offsetX = Lengthd(3.0, Lengthd::Unit::Px);
  shadow.offsetY = Lengthd(4.0, Lengthd::Unit::Px);
  shadow.stdDeviation = Lengthd(5.0, Lengthd::Unit::Px);
  shadow.color = css::Color(css::Color::CurrentColor{});

  instance.resolvedFilter = std::vector<FilterEffect>{
      FilterEffect(
          FilterEffect::Blur{Lengthd(2.0, Lengthd::Unit::Px), Lengthd(3.0, Lengthd::Unit::Px)}),
      FilterEffect(FilterEffect::HueRotate{90.0}),
      FilterEffect(FilterEffect::Brightness{1.25}),
      FilterEffect(FilterEffect::Contrast{0.5}),
      FilterEffect(FilterEffect::Grayscale{0.25}),
      FilterEffect(FilterEffect::Invert{0.2}),
      FilterEffect(FilterEffect::FilterOpacity{0.4}),
      FilterEffect(FilterEffect::Saturate{0.3}),
      FilterEffect(FilterEffect::Sepia{0.6}),
      FilterEffect(shadow),
  };

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushFilterLayer(_, _))
      .WillOnce([&](const components::FilterGraph& filterGraph,
                    const std::optional<Box2d>& filterRegion) {
        EXPECT_FALSE(filterRegion.has_value());
        EXPECT_EQ(filterGraph.colorInterpolationFilters, ColorInterpolationFilters::SRGB);
        ASSERT_EQ(filterGraph.nodes.size(), 10u);

        const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
            &filterGraph.nodes[0].primitive);
        ASSERT_NE(blur, nullptr);
        EXPECT_DOUBLE_EQ(blur->stdDeviationX, 2.0);
        EXPECT_DOUBLE_EQ(blur->stdDeviationY, 3.0);

        const auto* hue =
            std::get_if<components::filter_primitive::ColorMatrix>(&filterGraph.nodes[1].primitive);
        ASSERT_NE(hue, nullptr);
        EXPECT_EQ(hue->type, components::filter_primitive::ColorMatrix::Type::HueRotate);
        EXPECT_THAT(hue->values, testing::ElementsAre(90.0));

        const auto* brightness = std::get_if<components::filter_primitive::ComponentTransfer>(
            &filterGraph.nodes[2].primitive);
        ASSERT_NE(brightness, nullptr);
        EXPECT_EQ(brightness->funcR.type,
                  components::filter_primitive::ComponentTransfer::FuncType::Linear);
        EXPECT_DOUBLE_EQ(brightness->funcR.slope, 1.25);
        EXPECT_DOUBLE_EQ(brightness->funcR.intercept, 0.0);

        const auto* contrast = std::get_if<components::filter_primitive::ComponentTransfer>(
            &filterGraph.nodes[3].primitive);
        ASSERT_NE(contrast, nullptr);
        EXPECT_DOUBLE_EQ(contrast->funcR.slope, 0.5);
        EXPECT_DOUBLE_EQ(contrast->funcR.intercept, 0.25);

        const auto* grayscale =
            std::get_if<components::filter_primitive::ColorMatrix>(&filterGraph.nodes[4].primitive);
        ASSERT_NE(grayscale, nullptr);
        EXPECT_EQ(grayscale->type, components::filter_primitive::ColorMatrix::Type::Saturate);
        EXPECT_THAT(grayscale->values, testing::ElementsAre(0.75));

        const auto* invert = std::get_if<components::filter_primitive::ComponentTransfer>(
            &filterGraph.nodes[5].primitive);
        ASSERT_NE(invert, nullptr);
        EXPECT_EQ(invert->funcR.type,
                  components::filter_primitive::ComponentTransfer::FuncType::Table);
        EXPECT_THAT(invert->funcR.tableValues, testing::ElementsAre(0.2, 0.8));

        const auto* filterOpacity = std::get_if<components::filter_primitive::ComponentTransfer>(
            &filterGraph.nodes[6].primitive);
        ASSERT_NE(filterOpacity, nullptr);
        EXPECT_DOUBLE_EQ(filterOpacity->funcA.slope, 0.4);
        EXPECT_DOUBLE_EQ(filterOpacity->funcA.intercept, 0.0);

        const auto* saturate =
            std::get_if<components::filter_primitive::ColorMatrix>(&filterGraph.nodes[7].primitive);
        ASSERT_NE(saturate, nullptr);
        EXPECT_EQ(saturate->type, components::filter_primitive::ColorMatrix::Type::Saturate);
        EXPECT_THAT(saturate->values, testing::ElementsAre(0.3));

        const auto* sepia =
            std::get_if<components::filter_primitive::ColorMatrix>(&filterGraph.nodes[8].primitive);
        ASSERT_NE(sepia, nullptr);
        EXPECT_EQ(sepia->type, components::filter_primitive::ColorMatrix::Type::Matrix);
        ASSERT_EQ(sepia->values.size(), 20u);

        const auto* dropShadow =
            std::get_if<components::filter_primitive::DropShadow>(&filterGraph.nodes[9].primitive);
        ASSERT_NE(dropShadow, nullptr);
        EXPECT_DOUBLE_EQ(dropShadow->dx, 3.0);
        EXPECT_DOUBLE_EQ(dropShadow->dy, 4.0);
        EXPECT_DOUBLE_EQ(dropShadow->stdDeviationX, 5.0);
        EXPECT_DOUBLE_EQ(dropShadow->stdDeviationY, 5.0);
        EXPECT_EQ(dropShadow->floodColor, css::Color(css::RGBA(12, 34, 56, 255)));
        EXPECT_DOUBLE_EQ(dropShadow->floodOpacity, 1.0);
      });
  EXPECT_CALL(renderer, popFilterLayer()).Times(1);

  RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  driver.drawEntityRange(document.registry(), rectEntity, rectEntity, viewport, Transform2d());
}

TEST_F(RendererDriverTest, DrawEntityRangeUsesResolvedFilterReferenceObjectBoundingBoxRegion) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="10" y="20" width="40" height="10" fill="red" />
  )svg",
                                      Vector2i(200, 100));

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  ASSERT_FALSE(warnings.hasWarnings());

  Entity rectEntity = entt::null;
  for (auto view = document.registry().view<components::RenderingInstanceComponent>();
       const Entity entity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (instance.dataHandle(document.registry()).all_of<components::ComputedPathComponent>()) {
      rectEntity = entity;
      break;
    }
  }
  ASSERT_TRUE(rectEntity != entt::null);

  Entity filterEntity = document.registry().create();
  components::ComputedFilterComponent computedFilter;
  computedFilter.x = Lengthd(-0.25, Lengthd::Unit::None);
  computedFilter.y = Lengthd(-0.5, Lengthd::Unit::None);
  computedFilter.width = Lengthd(1.5, Lengthd::Unit::None);
  computedFilter.height = Lengthd(2.0, Lengthd::Unit::None);
  computedFilter.filterUnits = FilterUnits::ObjectBoundingBox;
  computedFilter.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
  computedFilter.filterGraph.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
  components::FilterNode blurNode;
  blurNode.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 1.0,
      .stdDeviationY = 2.0,
  };
  blurNode.inputs.push_back(components::FilterInput{});
  computedFilter.filterGraph.nodes.push_back(std::move(blurNode));
  document.registry().emplace<components::ComputedFilterComponent>(filterEntity,
                                                                   std::move(computedFilter));

  auto& instance = document.registry().get<components::RenderingInstanceComponent>(rectEntity);
  instance.resolvedFilter = ResolvedReference{EntityHandle(document.registry(), filterEntity)};

  std::vector<Transform2d> transforms;
  const Transform2d baseTransform = Transform2d::Translate(Vector2d(3, 4));

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_))
      .Times(AtLeast(1))
      .WillRepeatedly([&](const Transform2d& transform) { transforms.push_back(transform); });
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushFilterLayer(_, _))
      .WillOnce([&](const components::FilterGraph& filterGraph,
                    const std::optional<Box2d>& filterRegion) {
        ASSERT_TRUE(filterRegion.has_value());
        EXPECT_NEAR(filterRegion->topLeft.x, 0.0, 1e-6);
        EXPECT_NEAR(filterRegion->topLeft.y, 15.0, 1e-6);
        EXPECT_NEAR(filterRegion->width(), 60.0, 1e-6);
        EXPECT_NEAR(filterRegion->height(), 20.0, 1e-6);

        ASSERT_TRUE(filterGraph.elementBoundingBox.has_value());
        EXPECT_NEAR(filterGraph.elementBoundingBox->topLeft.x, 10.0, 1e-6);
        EXPECT_NEAR(filterGraph.elementBoundingBox->topLeft.y, 20.0, 1e-6);
        EXPECT_NEAR(filterGraph.elementBoundingBox->width(), 40.0, 1e-6);
        EXPECT_NEAR(filterGraph.elementBoundingBox->height(), 10.0, 1e-6);
        EXPECT_NEAR(filterGraph.userToPixelScale.x, 1.0, 1e-6);
        EXPECT_NEAR(filterGraph.userToPixelScale.y, 1.0, 1e-6);
      });
  EXPECT_CALL(renderer, popFilterLayer()).Times(1);

  RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  driver.drawEntityRange(document.registry(), rectEntity, rectEntity, viewport, baseTransform);

  EXPECT_THAT(transforms, testing::Contains(TransformNear(baseTransform, 1e-6)));
}

TEST_F(RendererDriverTest, DrawEntityRangeCopiesUrlFilterNodesIntoCssFilterGraph) {
  SVGDocument document = makeDocument(R"svg(
    <rect x="10" y="20" width="40" height="10" fill="red" />
  )svg",
                                      Vector2i(200, 100));

  ParseWarningSink warnings;
  RendererUtils::prepareDocumentForRendering(document, false, warnings);
  ASSERT_FALSE(warnings.hasWarnings());

  Entity rectEntity = entt::null;
  for (auto view = document.registry().view<components::RenderingInstanceComponent>();
       const Entity entity : view) {
    const auto& instance = view.get<components::RenderingInstanceComponent>(entity);
    if (instance.dataHandle(document.registry()).all_of<components::ComputedPathComponent>()) {
      rectEntity = entity;
      break;
    }
  }
  ASSERT_TRUE(rectEntity != entt::null);

  Entity filterEntity = document.registry().create();
  document.registry().emplace<components::IdComponent>(filterEntity, RcString("refFilter"));
  components::ComputedFilterComponent computedFilter;
  computedFilter.filterGraph.colorInterpolationFilters = ColorInterpolationFilters::LinearRGB;
  components::FilterNode referencedNode;
  referencedNode.primitive = components::filter_primitive::GaussianBlur{
      .stdDeviationX = 6.0,
      .stdDeviationY = 7.0,
  };
  referencedNode.inputs.push_back(components::FilterInput{});
  computedFilter.filterGraph.nodes.push_back(std::move(referencedNode));
  document.registry().emplace<components::ComputedFilterComponent>(filterEntity,
                                                                   std::move(computedFilter));

  auto& instance = document.registry().get<components::RenderingInstanceComponent>(rectEntity);
  instance.resolvedFilter = std::vector<FilterEffect>{
      FilterEffect(FilterEffect::ElementReference(Reference("#refFilter"))),
      FilterEffect(FilterEffect::HueRotate{45.0}),
  };

  EXPECT_CALL(renderer, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawPath(_, _)).Times(AtLeast(1));
  EXPECT_CALL(renderer, pushFilterLayer(_, _))
      .WillOnce([&](const components::FilterGraph& filterGraph,
                    const std::optional<Box2d>& filterRegion) {
        ASSERT_TRUE(filterRegion.has_value());
        EXPECT_NEAR(filterRegion->topLeft.x, 6.0, 1e-6);
        EXPECT_NEAR(filterRegion->topLeft.y, 19.0, 1e-6);
        EXPECT_NEAR(filterRegion->width(), 48.0, 1e-6);
        EXPECT_NEAR(filterRegion->height(), 12.0, 1e-6);

        ASSERT_EQ(filterGraph.nodes.size(), 2u);
        const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
            &filterGraph.nodes[0].primitive);
        ASSERT_NE(blur, nullptr);
        EXPECT_DOUBLE_EQ(blur->stdDeviationX, 6.0);
        EXPECT_DOUBLE_EQ(blur->stdDeviationY, 7.0);
        ASSERT_TRUE(filterGraph.nodes[0].colorInterpolationFilters.has_value());
        EXPECT_EQ(filterGraph.nodes[0].colorInterpolationFilters.value(),
                  ColorInterpolationFilters::LinearRGB);

        const auto* hue =
            std::get_if<components::filter_primitive::ColorMatrix>(&filterGraph.nodes[1].primitive);
        ASSERT_NE(hue, nullptr);
        EXPECT_THAT(hue->values, testing::ElementsAre(45.0));
      });
  EXPECT_CALL(renderer, popFilterLayer()).Times(1);

  RenderViewport viewport;
  viewport.size = Vector2d(200, 100);
  viewport.devicePixelRatio = 1.0;

  driver.drawEntityRange(document.registry(), rectEntity, rectEntity, viewport, Transform2d());
}

TEST_F(RendererDriverTest, ResolvesSpanFallbackPaintFromAncestorStyle) {
  SVGDocument document = makeDocument(R"svg(
    <text x="10" y="20">
      <tspan>Text</tspan>
    </text>
  )svg",
                                      Vector2i(120, 40));

  const css::RGBA fallbackFill(200, 10, 20, 255);
  const css::RGBA fallbackStroke(10, 120, 30, 255);

  EXPECT_CALL(renderer, beginFrame(_)).WillOnce([&](const RenderViewport&) {
    auto maybeText = document.querySelector("text");
    auto maybeTspan = document.querySelector("tspan");
    ASSERT_TRUE(maybeText.has_value());
    ASSERT_TRUE(maybeTspan.has_value());

    const Entity textEntity = maybeText->entityHandle().entity();
    const Entity tspanEntity = maybeTspan->entityHandle().entity();
    document.registry().remove<components::ComputedStyleComponent>(tspanEntity);

    auto& textStyle = document.registry().get<components::ComputedStyleComponent>(textEntity);
    ASSERT_TRUE(textStyle.properties.has_value());

    const css::Specificity inlineSpecificity = css::Specificity::FromABC(1, 0, 0);
    textStyle.properties->fill.set(
        PaintServer(PaintServer::ElementReference(Reference("#missing"), css::Color(fallbackFill))),
        inlineSpecificity);
    textStyle.properties->stroke.set(PaintServer(PaintServer::ElementReference(
                                         Reference("#missing"), css::Color(fallbackStroke))),
                                     inlineSpecificity);
  });

  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, drawText(_, _, _))
      .WillOnce([&](Registry&, const components::ComputedTextComponent& text, const TextParams&) {
        const auto spanIt =
            std::find_if(text.spans.begin(), text.spans.end(), [](const auto& span) {
              return std::string_view(span.text).substr(span.start, span.end - span.start) ==
                     "Text";
            });
        ASSERT_NE(spanIt, text.spans.end());
        const auto* fill = std::get_if<PaintServer::Solid>(&spanIt->resolvedFill);
        const auto* stroke = std::get_if<PaintServer::Solid>(&spanIt->resolvedStroke);
        ASSERT_NE(fill, nullptr);
        ASSERT_NE(stroke, nullptr);
        EXPECT_EQ(fill->color.resolve(css::RGBA(0, 0, 0, 255), 1.0f), fallbackFill);
        EXPECT_EQ(stroke->color.resolve(css::RGBA(0, 0, 0, 255), 1.0f), fallbackStroke);
      });

  driver.draw(document);
}

}  // namespace
}  // namespace donner::svg
