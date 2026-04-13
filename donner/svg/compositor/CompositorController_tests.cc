#include "donner/svg/compositor/CompositorController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/LayerMembershipComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;
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

class CompositorControllerTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  NiceMock<MockRendererInterface> renderer_;
};

TEST_F(CompositorControllerTest, ConstructsWithDocumentAndRenderer) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  CompositorController compositor(document, renderer_);
  EXPECT_EQ(compositor.layerCount(), 0u);
  EXPECT_EQ(compositor.totalBitmapMemory(), 0u);
}

TEST_F(CompositorControllerTest, PromoteEntitySucceeds) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_TRUE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);
}

TEST_F(CompositorControllerTest, PromoteSameEntityTwiceSucceeds) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_TRUE(compositor.promoteEntity(entity));  // Idempotent.
  EXPECT_EQ(compositor.layerCount(), 1u);
}

TEST_F(CompositorControllerTest, PromoteInvalidEntityFails) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(compositor.promoteEntity(entt::null));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, DemoteRemovesLayer) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);

  compositor.demoteEntity(entity);
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, DemoteNonPromotedEntityIsNoOp) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.demoteEntity(entity);  // No-op, should not crash.
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, LayerMembershipComponentAttachedOnPromote) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();
  Registry& registry = document.registry();

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(registry.all_of<LayerMembershipComponent>(entity));

  compositor.promoteEntity(entity);
  EXPECT_TRUE(registry.all_of<LayerMembershipComponent>(entity));

  compositor.demoteEntity(entity);
  EXPECT_FALSE(registry.all_of<LayerMembershipComponent>(entity));
}

TEST_F(CompositorControllerTest, LayerLimitEnforced) {
  // Create a document with many elements.
  std::string svgContent;
  for (int i = 0; i < kMaxCompositorLayers + 5; ++i) {
    svgContent +=
        "<rect id=\"r" + std::to_string(i) + "\" x=\"" + std::to_string(i) + "\" width=\"1\" "
        "height=\"1\" fill=\"red\" />\n";
  }

  SVGDocument document = makeDocument(svgContent, Vector2i(200, 200));
  CompositorController compositor(document, renderer_);

  // Promote up to the limit.
  for (int i = 0; i < kMaxCompositorLayers; ++i) {
    auto elem = document.querySelector("#r" + std::to_string(i));
    ASSERT_TRUE(elem.has_value()) << "Element r" << i << " not found";
    EXPECT_TRUE(compositor.promoteEntity(elem->entityHandle().entity()))
        << "Failed to promote element " << i;
  }

  EXPECT_EQ(compositor.layerCount(), static_cast<size_t>(kMaxCompositorLayers));

  // Next promotion should fail.
  auto extra = document.querySelector("#r" + std::to_string(kMaxCompositorLayers));
  ASSERT_TRUE(extra.has_value());
  EXPECT_FALSE(compositor.promoteEntity(extra->entityHandle().entity()));
}

TEST_F(CompositorControllerTest, PromoteMultipleEntities) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" width="10" height="10" fill="red" />
    <rect id="b" x="20" width="10" height="10" fill="blue" />
  )svg");

  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(a->entityHandle().entity()));
  EXPECT_TRUE(compositor.promoteEntity(b->entityHandle().entity()));
  EXPECT_EQ(compositor.layerCount(), 2u);

  EXPECT_TRUE(compositor.isPromoted(a->entityHandle().entity()));
  EXPECT_TRUE(compositor.isPromoted(b->entityHandle().entity()));
}

TEST_F(CompositorControllerTest, SetCompositionTransform) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);

  const Transform2d translate = Transform2d::Translate(5.0, 10.0);
  compositor.setLayerCompositionTransform(entity, translate);

  const Transform2d result = compositor.compositionTransformOf(entity);
  EXPECT_TRUE(result.isTranslation());
  EXPECT_NEAR(result.translation().x, 5.0, 1e-10);
  EXPECT_NEAR(result.translation().y, 10.0, 1e-10);
}

TEST_F(CompositorControllerTest, CompositionTransformOfNonPromotedReturnsIdentity) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  const Transform2d result = compositor.compositionTransformOf(target->entityHandle().entity());
  EXPECT_TRUE(result.isIdentity());
}

TEST_F(CompositorControllerTest, RenderFrameCallsRendererDraw) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  // The skeleton renderFrame delegates to RendererDriver::draw, which calls the renderer methods.
  EXPECT_CALL(renderer_, beginFrame(_)).Times(AtLeast(1));
  EXPECT_CALL(renderer_, endFrame()).Times(AtLeast(1));

  CompositorController compositor(document, renderer_);

  RenderViewport viewport;
  viewport.size = Vector2d(16, 16);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
}

TEST_F(CompositorControllerTest, MoveConstructor) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_EQ(compositor.layerCount(), 1u);

  CompositorController moved(std::move(compositor));
  EXPECT_EQ(moved.layerCount(), 1u);
  EXPECT_TRUE(moved.isPromoted(entity));
}

}  // namespace donner::svg::compositor
