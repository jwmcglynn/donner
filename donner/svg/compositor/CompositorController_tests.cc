#include "donner/svg/compositor/CompositorController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::NiceMock;

namespace donner::svg::compositor {

namespace {

using MockRendererInterface = tests::MockRendererInterface;

}  // namespace

class CompositorControllerTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  void configureMockForCaching() {
    ON_CALL(renderer_, takeSnapshot()).WillByDefault([]() {
      return MockRendererInterface::makeDummyBitmap();
    });
    ON_CALL(renderer_, createOffscreenInstance()).WillByDefault([this]() {
      auto offscreen = std::make_unique<NiceMock<MockRendererInterface>>();
      ON_CALL(*offscreen, takeSnapshot()).WillByDefault([]() {
        return MockRendererInterface::makeDummyBitmap();
      });
      ON_CALL(*offscreen, createOffscreenInstance()).WillByDefault([]() { return nullptr; });
      return offscreen;
    });
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

TEST_F(CompositorControllerTest, ComputedLayerAssignmentAttachedOnPromote) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();
  Registry& registry = document.registry();

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(registry.all_of<ComputedLayerAssignmentComponent>(entity));

  compositor.promoteEntity(entity);
  EXPECT_TRUE(registry.all_of<ComputedLayerAssignmentComponent>(entity));
  EXPECT_NE(registry.get<ComputedLayerAssignmentComponent>(entity).layerId, 0u);

  compositor.demoteEntity(entity);
  EXPECT_FALSE(registry.all_of<ComputedLayerAssignmentComponent>(entity));
}

TEST_F(CompositorControllerTest, LayerLimitEnforced) {
  // Create a document with many elements.
  std::string svgContent;
  for (int i = 0; i < kMaxCompositorLayers + 5; ++i) {
    svgContent += "<rect id=\"r" + std::to_string(i) + "\" x=\"" + std::to_string(i) +
                  "\" width=\"1\" "
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

TEST_F(CompositorControllerTest, LayerComposeOffsetTracksDomTranslationDelta) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  configureMockForCaching();
  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);

  // First frame: bitmap is stamped against the DOM's identity transform.
  RenderViewport viewport;
  viewport.size = Vector2d(100, 100);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);

  // Mutate the DOM — the compositor's fast path detects the pure-translation
  // delta and should report it via `layerComposeOffset()`.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(5.0, 10.0));
  compositor.renderFrame(viewport);

  const Transform2d result = compositor.layerComposeOffset(entity);
  EXPECT_TRUE(result.isTranslation());
  EXPECT_NEAR(result.translation().x, 5.0, 1e-10);
  EXPECT_NEAR(result.translation().y, 10.0, 1e-10);
}

TEST_F(CompositorControllerTest, LayerComposeOffsetOfNonPromotedReturnsIdentity) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  const Transform2d result = compositor.layerComposeOffset(target->entityHandle().entity());
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

TEST_F(CompositorControllerTest, SinglePromotedLayerBuildsSplitStaticLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="under" x="0" y="0" width="10" height="10" fill="blue" />
    <rect id="target" x="20" y="0" width="10" height="10" fill="red" />
    <rect id="over" x="40" y="0" width="10" height="10" fill="green" />
  )svg");

  configureMockForCaching();

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);

  EXPECT_TRUE(compositor.hasSplitStaticLayers());
  EXPECT_FALSE(compositor.backgroundBitmap().empty());
  EXPECT_FALSE(compositor.layerBitmapOf(entity).empty());
  EXPECT_FALSE(compositor.foregroundBitmap().empty());
}

TEST_F(CompositorControllerTest, MultiplePromotedLayersDoNotBuildSplitStaticLayers) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="10" height="10" fill="blue" />
    <rect id="b" x="20" y="0" width="10" height="10" fill="red" />
    <rect id="c" x="40" y="0" width="10" height="10" fill="green" />
  )svg");

  configureMockForCaching();

  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(a->entityHandle().entity()));
  ASSERT_TRUE(compositor.promoteEntity(b->entityHandle().entity()));

  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);

  EXPECT_FALSE(compositor.hasSplitStaticLayers());
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

TEST_F(CompositorControllerTest, SimpleFillNoFallback) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  // Must prepare document to populate RenderingInstanceComponent.
  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_EQ(compositor.fallbackReasonsOf(entity), FallbackReason::None);
}

TEST_F(CompositorControllerTest, FilterTriggersFallback) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <filter id="blur"><feGaussianBlur stdDeviation="5" /></filter>
    </defs>
    <rect id="target" width="10" height="10" fill="red" filter="url(#blur)" />
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_NE(compositor.fallbackReasonsOf(entity) & FallbackReason::Filter, FallbackReason::None);
}

TEST_F(CompositorControllerTest, ClipPathTriggersFallback) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <clipPath id="cp"><rect width="5" height="5" /></clipPath>
    </defs>
    <rect id="target" width="10" height="10" fill="red" clip-path="url(#cp)" />
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_NE(compositor.fallbackReasonsOf(entity) & FallbackReason::ClipPath, FallbackReason::None);
}

TEST_F(CompositorControllerTest, MaskTriggersFallback) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <mask id="m"><rect width="10" height="10" fill="white" /></mask>
    </defs>
    <rect id="target" width="10" height="10" fill="red" mask="url(#m)" />
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_NE(compositor.fallbackReasonsOf(entity) & FallbackReason::Mask, FallbackReason::None);
}

TEST_F(CompositorControllerTest, OpacityTriggersFallback) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" opacity="0.5" />
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_NE(compositor.fallbackReasonsOf(entity) & FallbackReason::IsolatedLayer,
            FallbackReason::None);
}

TEST_F(CompositorControllerTest, GradientFillTriggersFallback) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <linearGradient id="g">
        <stop offset="0" stop-color="red" />
        <stop offset="1" stop-color="blue" />
      </linearGradient>
    </defs>
    <rect id="target" width="10" height="10" fill="url(#g)" />
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.promoteEntity(entity);
  EXPECT_NE(compositor.fallbackReasonsOf(entity) & FallbackReason::ExternalPaint,
            FallbackReason::None);
}

TEST_F(CompositorControllerTest, FallbackReasonsOfUnpromotedEntity) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_EQ(compositor.fallbackReasonsOf(entity), FallbackReason::None);
}

TEST_F(CompositorControllerTest, ResetAllLayersClearsPromotedEntities) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="under" width="10" height="10" fill="blue" />
    <rect id="target" width="10" height="10" fill="red" />
    <rect id="over" width="10" height="10" fill="green" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  configureMockForCaching();

  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);
  EXPECT_TRUE(compositor.isPromoted(entity));

  // Render to populate bitmaps.
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  EXPECT_GT(compositor.totalBitmapMemory(), 0u);

  // Reset should clear everything.
  compositor.resetAllLayers();
  EXPECT_EQ(compositor.layerCount(), 0u);
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.totalBitmapMemory(), 0u);
}

TEST_F(CompositorControllerTest, ResetAllLayersClearsComputedLayerAssignment) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));

  // Verify component was added.
  EXPECT_TRUE(document.registry().all_of<ComputedLayerAssignmentComponent>(entity));
  EXPECT_NE(document.registry().get<ComputedLayerAssignmentComponent>(entity).layerId, 0u);

  compositor.resetAllLayers();

  // Verify component was removed.
  EXPECT_FALSE(document.registry().all_of<ComputedLayerAssignmentComponent>(entity));
}

TEST_F(CompositorControllerTest, ResetAllLayersAllowsRepromotion) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->entityHandle().entity();

  CompositorController compositor(document, renderer_);
  configureMockForCaching();

  EXPECT_TRUE(compositor.promoteEntity(entity));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  compositor.resetAllLayers();
  EXPECT_EQ(compositor.layerCount(), 0u);

  // Re-promote after reset should work.
  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);
  EXPECT_TRUE(compositor.isPromoted(entity));
}

}  // namespace donner::svg::compositor
