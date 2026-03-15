#include "donner/svg/renderer/RendererDriver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "donner/css/Specificity.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
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
  const Transformd identity;
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
    if (commands[i].type != expected[i]) {
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
  MOCK_METHOD(void, beginFrame, (const RenderViewport& viewport), (override));
  MOCK_METHOD(void, endFrame, (), (override));
  MOCK_METHOD(void, setTransform, (const Transformd& transform), (override));
  MOCK_METHOD(void, pushTransform, (const Transformd& transform), (override));
  MOCK_METHOD(void, popTransform, (), (override));
  MOCK_METHOD(void, pushClip, (const ResolvedClip& clip), (override));
  MOCK_METHOD(void, popClip, (), (override));
  MOCK_METHOD(void, pushIsolatedLayer, (double opacity, MixBlendMode blendMode), (override));
  MOCK_METHOD(void, popIsolatedLayer, (), (override));
  MOCK_METHOD(void, pushFilterLayer,
              (const components::FilterGraph& filterGraph, const std::optional<Boxd>& filterRegion),
              (override));
  MOCK_METHOD(void, popFilterLayer, (), (override));
  MOCK_METHOD(void, pushMask, (const std::optional<Boxd>& maskBounds), (override));
  MOCK_METHOD(void, transitionMaskToContent, (), (override));
  MOCK_METHOD(void, popMask, (), (override));
  MOCK_METHOD(void, beginPatternTile, (const Boxd& tileRect, const Transformd& targetFromPattern),
              (override));
  MOCK_METHOD(void, endPatternTile, (bool forStroke), (override));
  MOCK_METHOD(void, setPaint, (const PaintParams& paint), (override));
  MOCK_METHOD(void, drawPath, (const PathShape& path, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawRect, (const Boxd& rect, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawEllipse, (const Boxd& bounds, const StrokeParams& stroke), (override));
  MOCK_METHOD(void, drawImage, (const ImageResource& image, const ImageParams& params), (override));
  MOCK_METHOD(void, drawText,
              (const components::ComputedTextComponent& text, const TextParams& params),
              (override));
  MOCK_METHOD(RendererBitmap, takeSnapshot, (), (const, override));
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

  const std::array<PathSpline::CommandType, 5> rectCommands = {
      PathSpline::CommandType::MoveTo, PathSpline::CommandType::LineTo,
      PathSpline::CommandType::LineTo, PathSpline::CommandType::LineTo,
      PathSpline::CommandType::ClosePath};

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
    textInstance.entityFromWorldTransform = Transformd();
    textInstance.resolvedFill = PaintServer::Solid(css::Color(css::RGBA::RGB(0, 255, 0)));

    const Vector2i canvasSize = document.canvasSize();
    document.registry().emplace<components::ComputedViewBoxComponent>(
        textEntity, Boxd(Vector2d(), Vector2d(static_cast<double>(canvasSize.x),
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
    text.spans.push_back(components::ComputedTextComponent::TextSpan{
        RcString("hi"), 0, 2, Lengthd(2, Lengthd::Unit::None), Lengthd(3, Lengthd::Unit::None),
        Lengthd(), Lengthd(), 0.0});
    document.registry().emplace<components::ComputedTextComponent>(textEntity, text);
  });
  EXPECT_CALL(renderer, endFrame()).Times(1);
  EXPECT_CALL(renderer, setTransform(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer, setPaint(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer,
              drawText(_, Field(&TextParams::fillColor, Eq(css::Color(css::RGBA(0, 255, 0, 255))))))
      .Times(AtLeast(1));

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

  std::vector<Transformd> transforms;
  EXPECT_CALL(renderer, pushTransform(_))
      .Times(testing::AtLeast(1))
      .WillRepeatedly([&](const Transformd& transform) { transforms.push_back(transform); });

  const Boxd bounds = Boxd::FromXYWH(2, 0, 6, 4);

  for (const Entity entity : images) {
    document.registry().remove<components::PreserveAspectRatioComponent>(entity);

    auto& loaded = document.registry().emplace<components::LoadedImageComponent>(entity);
    loaded.image = ImageResource{};
    loaded.image->width = 3;
    loaded.image->height = 2;
    loaded.image->data = std::vector<uint8_t>(
        static_cast<size_t>(loaded.image->width * loaded.image->height * 4), 255);

    const Boxd intrinsicSize = Boxd::WithSize(Vector2d(loaded.image->width, loaded.image->height));
    const Transformd expectedTransform =
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

  const Boxd intrinsicSize = Boxd::WithSize(Vector2d(3, 2));
  const Transformd expectedTransform =
      PreserveAspectRatio::Default().elementContentFromViewBoxTransform(bounds, intrinsicSize);
  EXPECT_THAT(transforms, testing::Contains(TransformNear(expectedTransform, 1e-6)));
}

}  // namespace
}  // namespace donner::svg
