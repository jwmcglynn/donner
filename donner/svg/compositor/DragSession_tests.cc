#include "donner/svg/compositor/DragSession.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/LayerMembershipComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::NiceMock;

namespace donner::svg::compositor {

namespace {

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

}  // namespace

class DragSessionTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  NiceMock<MockRendererInterface> renderer_;
};

TEST_F(DragSessionTest, BeginPromotesEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  auto session = DragSession::begin(compositor, entity);
  ASSERT_TRUE(session.has_value());
  EXPECT_TRUE(session->isActive());
  EXPECT_TRUE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);
}

TEST_F(DragSessionTest, EndDemotesEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  auto session = DragSession::begin(compositor, entity);
  ASSERT_TRUE(session.has_value());

  session->end();
  EXPECT_FALSE(session->isActive());
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(DragSessionTest, DestructorDemotes) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  {
    auto session = DragSession::begin(compositor, entity);
    ASSERT_TRUE(session.has_value());
    EXPECT_TRUE(compositor.isPromoted(entity));
  }
  // Session destroyed — entity should be demoted.
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(DragSessionTest, UpdateTranslation) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  auto session = DragSession::begin(compositor, entity);
  ASSERT_TRUE(session.has_value());

  session->updateTranslation(Vector2d(15.0, 25.0));
  Transform2d result = compositor.compositionTransformOf(entity);
  EXPECT_TRUE(result.isTranslation());
  EXPECT_NEAR(result.translation().x, 15.0, 1e-10);
  EXPECT_NEAR(result.translation().y, 25.0, 1e-10);
}

TEST_F(DragSessionTest, BeginWithInvalidEntityFails) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  CompositorController compositor(document, renderer_);
  auto session = DragSession::begin(compositor, entt::null);
  EXPECT_FALSE(session.has_value());
}

TEST_F(DragSessionTest, MoveConstructor) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  auto session = DragSession::begin(compositor, entity);
  ASSERT_TRUE(session.has_value());

  DragSession moved(std::move(*session));
  EXPECT_TRUE(moved.isActive());
  EXPECT_FALSE(session->isActive());  // NOLINT: intentional use-after-move
  EXPECT_TRUE(compositor.isPromoted(entity));

  moved.end();
  EXPECT_FALSE(compositor.isPromoted(entity));
}

TEST_F(DragSessionTest, MoveAssignment) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" width="10" height="10" fill="red" />
    <rect id="b" x="20" width="10" height="10" fill="blue" />
  )svg");

  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Entity entityA = a->entityHandle().entity();
  const Entity entityB = b->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  auto sessionA = DragSession::begin(compositor, entityA);
  auto sessionB = DragSession::begin(compositor, entityB);
  ASSERT_TRUE(sessionA.has_value());
  ASSERT_TRUE(sessionB.has_value());

  // Move-assign B over A — should demote A.
  *sessionA = std::move(*sessionB);
  EXPECT_FALSE(compositor.isPromoted(entityA));  // A was demoted by assignment.
  EXPECT_TRUE(compositor.isPromoted(entityB));   // B is still promoted via sessionA.
}

}  // namespace donner::svg::compositor
