#include "donner/svg/compositor/CompositorController.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ostream>
#include <string_view>
#include <thread>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/tests/ParserTestUtils.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::HasSubstr;
using ::testing::NiceMock;

namespace donner::svg::compositor {

void PrintTo(StaticSpanMode mode, std::ostream* os) {
  switch (mode) {
    case StaticSpanMode::CachedTile: *os << "CachedTile"; return;
    case StaticSpanMode::Immediate: *os << "Immediate"; return;
  }
  *os << "StaticSpanMode(" << static_cast<int>(mode) << ")";
}

void PrintTo(const CompositorController::StaticSpanPlan& plan, std::ostream* os) {
  *os << "StaticSpanPlan{slotIndex=" << plan.slotIndex
      << ", mode=" << testing::PrintToString(plan.mode) << ", visible=" << plan.visible
      << ", expensive=" << plan.hasExpensiveEffect << ", drawOps=" << plan.estimatedDrawOps
      << ", pathVerbs=" << plan.estimatedPathVerbs
      << ", retainedBytes=" << plan.estimatedRetainedBytes
      << ", estimatedRasterizeMs=" << plan.estimatedRasterizeMs
      << ", measuredRasterizeMs=" << plan.measuredRasterizeMs
      << ", immediateBudgetMs=" << plan.immediateBudgetMs
      << ", staticImmediate=" << plan.staticHeuristicImmediate
      << ", dynamicImmediate=" << plan.dynamicHeuristicImmediate
      << ", demotedDynamicImmediate=" << plan.demotedDynamicImmediate
      << ", label=" << testing::PrintToString(plan.spanRangeLabel) << "}";
}

namespace {

using MockRendererInterface = tests::MockRendererInterface;
using LayerInspectorRow = CompositorController::LayerInspectorRow;
using StaticSpanPlan = CompositorController::StaticSpanPlan;

MATCHER(EstimatedRedrawCostNoMoreThanCacheOverhead, "") {
  *result_listener << "estimatedRedrawCost=" << arg.estimatedRedrawCost
                   << ", estimatedCacheOverheadCost=" << arg.estimatedCacheOverheadCost;
  return arg.estimatedRedrawCost <= arg.estimatedCacheOverheadCost;
}

MATCHER(EstimatedRasterizeWithinImmediateBudget, "") {
  *result_listener << "estimatedRasterizeMs=" << arg.estimatedRasterizeMs
                   << ", immediateBudgetMs=" << arg.immediateBudgetMs;
  return arg.estimatedRasterizeMs <= arg.immediateBudgetMs;
}

MATCHER(MeasuredRasterizeExceedsImmediateBudget, "") {
  *result_listener << "measuredRasterizeMs=" << arg.measuredRasterizeMs
                   << ", immediateBudgetMs=" << arg.immediateBudgetMs;
  return arg.measuredRasterizeMs > arg.immediateBudgetMs;
}

template <typename... Matchers>
auto StaticSpanPlanAtSlot(size_t slotIndex, Matchers... matchers) {
  return ::testing::AllOf(::testing::Field("slotIndex", &StaticSpanPlan::slotIndex, slotIndex),
                          matchers...);
}

auto LayerRasterizeCountIs(auto matcher) {
  return ::testing::Field("rasterizeCount", &LayerInspectorRow::rasterizeCount, matcher);
}

auto LayerGenerationIs(auto matcher) {
  return ::testing::Field("generation", &LayerInspectorRow::generation, matcher);
}

auto LayerHasValidBitmapIs(auto matcher) {
  return ::testing::Field("hasValidBitmap", &LayerInspectorRow::hasValidBitmap, matcher);
}

auto LayerDirtyIs(auto matcher) {
  return ::testing::Field("dirty", &LayerInspectorRow::dirty, matcher);
}

auto LayerBitmapSizeIs(auto matcher) {
  return ::testing::Field("bitmapSize", &LayerInspectorRow::bitmapSize, matcher);
}

auto LayerThumbnailDimsIs(auto matcher) {
  return ::testing::Field("thumbnailDims", &LayerInspectorRow::thumbnailDims, matcher);
}

auto LayerThumbnailPixelsAre(auto matcher) {
  return ::testing::Field("thumbnailPixels", &LayerInspectorRow::thumbnailPixels, matcher);
}

class FakeTextureSnapshot : public RendererTextureSnapshot {
public:
  explicit FakeTextureSnapshot(Vector2i dimensions) : dimensions_(dimensions) {}

  [[nodiscard]] RendererTextureSnapshotBackend backend() const override {
    return RendererTextureSnapshotBackend::Geode;
  }
  [[nodiscard]] Vector2i dimensions() const override { return dimensions_; }
  [[nodiscard]] AlphaType alphaType() const override { return AlphaType::Premultiplied; }

private:
  Vector2i dimensions_;
};

}  // namespace

class CompositorControllerTest : public ::testing::Test {
protected:
  SVGDocument makeDocument(std::string_view svg, Vector2i size = kTestSvgDefaultSize) {
    return instantiateSubtree(svg, parser::SVGParser::Options(), size);
  }

  void configureMockForCaching(
      std::chrono::milliseconds snapshotDelay = std::chrono::milliseconds(0)) {
    const auto makeBitmap = [snapshotDelay]() {
      if (snapshotDelay.count() > 0) {
        std::this_thread::sleep_for(snapshotDelay);
      }
      return MockRendererInterface::makeDummyBitmap();
    };
    ON_CALL(renderer_, takeSnapshot()).WillByDefault(makeBitmap);
    ON_CALL(renderer_, createOffscreenInstance()).WillByDefault([makeBitmap]() {
      auto offscreen = std::make_unique<NiceMock<MockRendererInterface>>();
      ON_CALL(*offscreen, takeSnapshot()).WillByDefault(makeBitmap);
      ON_CALL(*offscreen, createOffscreenInstance()).WillByDefault([]() { return nullptr; });
      return offscreen;
    });
  }

  void configureMockForTextureCaching() {
    ON_CALL(renderer_, requiresTextureSnapshotPresentation())
        .WillByDefault(::testing::Return(true));
    ON_CALL(renderer_, drawTextureSnapshot(_, _, _, _)).WillByDefault(::testing::Return(true));
    ON_CALL(renderer_, createOffscreenInstance()).WillByDefault([]() {
      auto offscreen = std::make_unique<NiceMock<MockRendererInterface>>();
      ON_CALL(*offscreen, requiresTextureSnapshotPresentation())
          .WillByDefault(::testing::Return(true));
      ON_CALL(*offscreen, takeTextureSnapshot()).WillByDefault([]() {
        return std::make_shared<FakeTextureSnapshot>(Vector2i(32, 32));
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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const auto result = compositor.promoteEntity(entt::null);
  EXPECT_EQ(result, CompositorController::PromoteResult::InvalidEntity);
  EXPECT_FALSE(result.promotedLayer());
  EXPECT_FALSE(result.fullCanvasPreviewRequired());
  EXPECT_EQ(compositor.layerCount(), 0u);
  EXPECT_FALSE(compositor.isPromoted(entt::null));
  EXPECT_TRUE(compositor.layerComposeOffset(entt::null).isIdentity());
  EXPECT_EQ(compositor.fallbackReasonsOf(entt::null), FallbackReason::None);
  EXPECT_TRUE(compositor.layerBitmapOf(entt::null).empty());
  EXPECT_EQ(compositor.layerCanvasOffsetOf(entt::null), Vector2d::Zero());

  const auto state = compositor.snapshotState();
  EXPECT_EQ(state.lastPromoteRefusalReason,
            CompositorController::PromoteRefusalReason::InvalidEntity);
  EXPECT_TRUE(state.lastPromoteRefusalEntity == entt::null);
}

TEST_F(CompositorControllerTest, DemoteRemovesLayer) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));
  EXPECT_EQ(compositor.layerCount(), 1u);

  compositor.demoteEntity(entity);
  // §M9: demote is queued; layer + hint linger until the hysteresis
  // window expires. Flush immediately so we can assert the
  // committed-demote state here.
  compositor.flushPendingDemotionsForTesting();
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

// Design doc 0033 §M9 — layer-set hysteresis. `demoteEntity` queues the
// demotion for `kDemotionHysteresisFrames` and only fires after the
// counter expires. A `promoteEntity` for the same entity inside the
// window cancels the queued demotion and reuses the cached layer
// (no `resyncSegmentsToLayerSet` rebuild).
TEST_F(CompositorControllerTest, M9DemoteIsLazyWithinHysteresisWindow) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  ASSERT_EQ(compositor.layerCount(), 1u);

  compositor.demoteEntity(entity);
  // The hysteresis hasn't expired — the layer + hint linger so the
  // editor's GL textures stay live and a re-promote can short-circuit.
  EXPECT_EQ(compositor.layerCount(), 1u);
  EXPECT_TRUE(compositor.isPromoted(entity));
}

TEST_F(CompositorControllerTest, M9RepromoteSameEntityCancelsPendingDemote) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  ASSERT_EQ(compositor.layerCount(), 1u);
  const uint64_t generationAfterFirstRender =
      compositor.snapshotLayerInspectorRows().front().generation;

  // Demote → re-promote with a different kind, mirroring the editor's
  // selection → drag transition. With the M9 fast path the layer is
  // preserved and its cached bitmap reused (generation does NOT
  // bump).
  compositor.demoteEntity(entity);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));
  ASSERT_TRUE(compositor.isPromoted(entity));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const uint64_t generationAfterRepromote =
      compositor.snapshotLayerInspectorRows().front().generation;
  EXPECT_EQ(generationAfterRepromote, generationAfterFirstRender)
      << "M9 fast path should reuse the cached bitmap — re-rasterizing on every "
         "click-deselect-click is exactly the work the hysteresis prevents.";
}

TEST_F(CompositorControllerTest, M9DemoteFiresAfterHysteresisExpires) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  ASSERT_EQ(compositor.layerCount(), 1u);

  compositor.demoteEntity(entity);
  EXPECT_EQ(compositor.layerCount(), 1u);

  // Render `kDemotionHysteresisFrames` frames — each call ages the
  // counter by one. The last call should expire the queue and run
  // the deferred resolver/reconcile that actually drops the layer.
  for (uint32_t i = 0; i < CompositorController::kDemotionHysteresisFrames + 1; ++i) {
    compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  }

  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

// Design doc 0033 §M9 + §M2C — pending-demote entries must NOT make
// `hasSplitStaticLayers()` return false during the hysteresis window.
// `skipMainCompose` gates on `hasSplitStaticLayers()`; if it returns
// false, `composeLayers` runs every fast-path drag frame, doing 2N+1
// canvas-scale bitmap blits per render. Operator observation on a
// selection-change drag at high zoom: "fast path counter increments
// but framerate stays low, scales worse with zoom".
TEST_F(CompositorControllerTest, M9PendingDemoteKeepsHasSplitStaticLayersTrue) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="10" height="10" fill="red" />
    <rect id="b" x="20" y="0" width="10" height="10" fill="blue" />
  )svg");

  configureMockForCaching();
  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Entity entityA = a->unsafeEntityHandle().entity();
  const Entity entityB = b->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entityA));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  ASSERT_TRUE(compositor.hasSplitStaticLayers())
      << "Sanity: single live promote should engage split-static-layers.";

  compositor.demoteEntity(entityA);
  ASSERT_TRUE(compositor.promoteEntity(entityB));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  // The live drag-target is B; A is pending-demote. Without this fix,
  // activeHints_.size() == 2 made hasSplitStaticLayers() return false
  // for the whole hysteresis window, disabling `skipMainCompose`.
  EXPECT_TRUE(compositor.hasSplitStaticLayers())
      << "Pending-demote A must not mask the live promote B from "
         "hasSplitStaticLayers() — would otherwise force composeLayers "
         "to run every fast-path drag frame.";
}

// Design doc 0033 §M9 + §M2C — pending-demote entries must NOT keep
// the live drag-target tile from being flagged `isDragTarget`. Before
// this fix, the `activeHints_.size() == 1` check in `renderFrame`'s
// `splitStaticLayersEntity_` setter saw `{old-pending-demote, new-
// drag-target}.size() == 2` during the hysteresis window and fell to
// `entt::null` — the worker's tile snapshot stopped emitting a
// dragTranslationDoc for the live target, and the editor blitted the
// content at the pre-drag position while the overlay (driven by the
// live DOM) moved with the cursor. Symptom: ~5–7s of "content stays
// put while overlay tracks the cursor" until the hysteresis expires.
TEST_F(CompositorControllerTest, M9PendingDemoteDoesNotMaskLiveDragTarget) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="10" height="10" fill="red" />
    <rect id="b" x="20" y="0" width="10" height="10" fill="blue" />
  )svg");

  configureMockForCaching();
  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Entity entityA = a->unsafeEntityHandle().entity();
  const Entity entityB = b->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entityA));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  // Editor switches drag-target: demote A (queued by M9), promote B.
  // activeHints_ now contains both A (pending) and B (live).
  compositor.demoteEntity(entityA);
  ASSERT_TRUE(compositor.promoteEntity(entityB));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  // The live drag-target tile (B) must be flagged `isDragTarget` so
  // the worker's `dragTranslationDoc` extraction propagates B's
  // `canvasFromBitmap` translation into the editor blit. Without
  // this guard the post-M9 `activeHints_.size() == 1` check would
  // count 2 hints and fall back to `entt::null` for the duration of
  // the hysteresis window.
  const auto tiles = compositor.snapshotTilesForUpload();
  bool sawLiveDragTarget = false;
  for (const auto& tile : tiles) {
    if (tile.layerEntity == entityB) {
      EXPECT_TRUE(tile.isDragTarget) << "live drag-target tile must be flagged isDragTarget";
      sawLiveDragTarget = true;
    } else if (tile.layerEntity == entityA) {
      EXPECT_FALSE(tile.isDragTarget) << "pending-demote tile must NOT be flagged isDragTarget";
    }
  }
  EXPECT_TRUE(sawLiveDragTarget);
}

TEST_F(CompositorControllerTest, M9FlushPendingDemotionsForTestingFiresImmediately) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.demoteEntity(entity);
  ASSERT_EQ(compositor.layerCount(), 1u);

  compositor.flushPendingDemotionsForTesting();
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, DemoteNonPromotedEntityIsNoOp) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  compositor.demoteEntity(entity);  // No-op, should not crash.
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, DemoteDetachedPromotedEntityClearsInteractionHint) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  ASSERT_TRUE(compositor.isPromoted(entity));

  target->remove();
  compositor.demoteEntity(entity);

  EXPECT_FALSE(compositor.isPromoted(entity));
  const auto state = compositor.snapshotState();
  EXPECT_EQ(state.activeHintsCount, 0u);
  EXPECT_TRUE(state.splitStaticLayersEntity == entt::null);
}

TEST_F(CompositorControllerTest, ComputedLayerAssignmentAttachedOnPromote) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();
  Registry& registry = document.registry();

  CompositorController compositor(document, renderer_);
  EXPECT_FALSE(registry.all_of<ComputedLayerAssignmentComponent>(entity));

  compositor.promoteEntity(entity);
  EXPECT_TRUE(registry.all_of<ComputedLayerAssignmentComponent>(entity));
  EXPECT_NE(registry.get<ComputedLayerAssignmentComponent>(entity).layerId, 0u);

  compositor.demoteEntity(entity);
  // §M9: flush the hysteresis queue so the assignment teardown
  // happens before we check.
  compositor.flushPendingDemotionsForTesting();
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
    EXPECT_TRUE(compositor.promoteEntity(elem->unsafeEntityHandle().entity()))
        << "Failed to promote element " << i;
  }

  EXPECT_EQ(compositor.layerCount(), static_cast<size_t>(kMaxCompositorLayers));

  // Next promotion should fail.
  auto extra = document.querySelector("#r" + std::to_string(kMaxCompositorLayers));
  ASSERT_TRUE(extra.has_value());
  EXPECT_FALSE(compositor.promoteEntity(extra->unsafeEntityHandle().entity()));
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
  EXPECT_TRUE(compositor.promoteEntity(a->unsafeEntityHandle().entity()));
  EXPECT_TRUE(compositor.promoteEntity(b->unsafeEntityHandle().entity()));
  EXPECT_EQ(compositor.layerCount(), 2u);

  EXPECT_TRUE(compositor.isPromoted(a->unsafeEntityHandle().entity()));
  EXPECT_TRUE(compositor.isPromoted(b->unsafeEntityHandle().entity()));
}

TEST_F(CompositorControllerTest, LayerComposeOffsetTracksDomTranslationDelta) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Transform2d result = compositor.layerComposeOffset(target->unsafeEntityHandle().entity());
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
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);

  // Post design-doc 0033 §M2C: `hasSplitStaticLayers` reports the
  // editor-promoted single-drag-target state, no bg/fg flatten step
  // exists. Assert the layer's bitmap is non-empty (the editor reads
  // it directly via `snapshotTilesForUpload`).
  EXPECT_TRUE(compositor.hasSplitStaticLayers());
  EXPECT_FALSE(compositor.layerBitmapOf(entity).empty());
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
  ASSERT_TRUE(compositor.promoteEntity(a->unsafeEntityHandle().entity()));
  ASSERT_TRUE(compositor.promoteEntity(b->unsafeEntityHandle().entity()));

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_EQ(compositor.fallbackReasonsOf(entity), FallbackReason::None);
}

TEST_F(CompositorControllerTest,
       PromoteDescendantUnderRawCompositingAncestorUsesFullCanvasPreview) {
  const auto expectFullCanvasPreview = [this](std::string_view svg) {
    SVGDocument document = makeDocument(svg);
    auto target = document.querySelector("#target");
    ASSERT_TRUE(target.has_value());
    const Entity entity = target->unsafeEntityHandle().entity();

    CompositorController compositor(document, renderer_);
    const auto result = compositor.promoteEntity(entity);

    EXPECT_EQ(result, CompositorController::PromoteResult::FullCanvasPreviewRequired);
    EXPECT_TRUE(result.fullCanvasPreviewRequired());
    EXPECT_FALSE(result.promotedLayer());
    EXPECT_FALSE(compositor.isPromoted(entity));
    EXPECT_EQ(compositor.layerCount(), 0u);
    EXPECT_EQ(compositor.snapshotState().lastPromoteRefusalReason,
              CompositorController::PromoteRefusalReason::None);
  };

  expectFullCanvasPreview(R"svg(
    <defs>
      <filter id="blur"><feGaussianBlur stdDeviation="2" /></filter>
    </defs>
    <g filter="url(#blur)">
      <rect id="target" width="10" height="10" fill="red" />
    </g>
  )svg");
  expectFullCanvasPreview(R"svg(
    <defs>
      <clipPath id="cp"><rect width="5" height="5" /></clipPath>
    </defs>
    <g clip-path="url(#cp)">
      <rect id="target" width="10" height="10" fill="red" />
    </g>
  )svg");
  expectFullCanvasPreview(R"svg(
    <defs>
      <mask id="m"><rect width="10" height="10" fill="white" /></mask>
    </defs>
    <g mask="url(#m)">
      <rect id="target" width="10" height="10" fill="red" />
    </g>
  )svg");
}

TEST_F(CompositorControllerTest,
       PromoteDescendantUnderPreparedIsolatedAncestorUsesFullCanvasPreview) {
  SVGDocument document = makeDocument(R"svg(
    <g id="ancestor" opacity="0.5">
      <rect id="target" width="10" height="10" fill="red" />
    </g>
  )svg");

  ParseWarningSink warningSink;
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warningSink);

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  const auto result = compositor.promoteEntity(entity);

  EXPECT_EQ(result, CompositorController::PromoteResult::FullCanvasPreviewRequired);
  EXPECT_TRUE(result.fullCanvasPreviewRequired());
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_EQ(compositor.layerCount(), 0u);
}

TEST_F(CompositorControllerTest, PromoteParentWithAlreadyPromotedDescendantFails) {
  SVGDocument document = makeDocument(R"svg(
    <g id="parent">
      <rect id="child" width="10" height="10" fill="red" opacity="0.5" />
    </g>
  )svg");

  configureMockForCaching();
  CompositorController compositor(document, renderer_);
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  auto parent = document.querySelector("#parent");
  auto child = document.querySelector("#child");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  const Entity parentEntity = parent->unsafeEntityHandle().entity();
  const Entity childEntity = child->unsafeEntityHandle().entity();
  ASSERT_NE(compositor.findLayerForTest(childEntity), nullptr)
      << "Sanity check: the opacity child should be mandatorily promoted first.";

  const auto result = compositor.promoteEntity(parentEntity);

  EXPECT_EQ(result, CompositorController::PromoteResult::DescendantPromoted);
  EXPECT_FALSE(compositor.isPromoted(parentEntity));
  EXPECT_NE(compositor.findLayerForTest(childEntity), nullptr);
  const auto state = compositor.snapshotState();
  EXPECT_EQ(state.lastPromoteRefusalReason,
            CompositorController::PromoteRefusalReason::DescendantPromoted);
  EXPECT_EQ(state.lastPromoteRefusalEntity, parentEntity);
}

TEST_F(CompositorControllerTest, ResetAllLayersClearsPromotedEntities) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="under" width="10" height="10" fill="blue" />
    <rect id="target" width="10" height="10" fill="red" />
    <rect id="over" width="10" height="10" fill="green" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

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
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.promoteEntity(entity));

  // Verify component was added.
  EXPECT_TRUE(document.registry().all_of<ComputedLayerAssignmentComponent>(entity));
  EXPECT_NE(document.registry().get<ComputedLayerAssignmentComponent>(entity).layerId, 0u);

  compositor.resetAllLayers();

  // Verify component was removed.
  EXPECT_FALSE(document.registry().all_of<ComputedLayerAssignmentComponent>(entity));
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsIsEmptyBeforePromotion) {
  SVGDocument document = makeDocument(R"svg(
    <rect width="10" height="10" fill="red" />
  )svg");

  CompositorController compositor(document, renderer_);
  EXPECT_TRUE(compositor.snapshotLayerInspectorRows().empty());
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsEmitsRowPerLayerWithBitmapDims) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="10" height="10" fill="blue" />
    <rect id="b" x="20" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto a = document.querySelector("#a");
  auto b = document.querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Entity entityA = a->unsafeEntityHandle().entity();
  const Entity entityB = b->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entityA));
  ASSERT_TRUE(compositor.promoteEntity(entityB));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rows = compositor.snapshotLayerInspectorRows();
  ASSERT_EQ(rows.size(), 2u);

  // Every row must point at one of the promoted entities; every row must
  // have a populated bitmap (the mock renderer's dummy bitmap) and a
  // non-zero rasterize count after the first renderFrame.
  std::vector<Entity> entitiesInRows;
  for (const auto& row : rows) {
    EXPECT_TRUE(row.hasValidBitmap);
    EXPECT_NE(row.bitmapSize, Vector2i::Zero());
    EXPECT_GE(row.rasterizeCount, 1u);
    EXPECT_GE(row.generation, 1u);
    entitiesInRows.push_back(row.entity);
  }
  EXPECT_THAT(entitiesInRows, ::testing::UnorderedElementsAre(entityA, entityB));
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsTracksRasterizeCountAcrossFrames) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="0" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  // First frame: layer must rasterize exactly once.
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  const auto rowsAfterFirst = compositor.snapshotLayerInspectorRows();
  EXPECT_THAT(rowsAfterFirst, ::testing::ElementsAre(LayerRasterizeCountIs(1u)));

  // A pure-translation drag goes through the fast path — bitmap is reused,
  // rasterize count does NOT advance.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(5.0, 0.0));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  const auto rowsAfterTranslate = compositor.snapshotLayerInspectorRows();
  EXPECT_THAT(rowsAfterTranslate, ::testing::ElementsAre(LayerRasterizeCountIs(1u)))
      << "Pure-translation drag must reuse the cached bitmap rather than re-rasterize.";
}

TEST_F(CompositorControllerTest, TextureOnlyDragReusesPayloadWithoutRasterize) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="0" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForTextureCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));

  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  ASSERT_TRUE(compositor.hasSplitStaticLayers())
      << "Texture-backed drag layers must count as cached split layers.";
  const auto rowsAfterFirst = compositor.snapshotLayerInspectorRows();
  ASSERT_THAT(rowsAfterFirst, ::testing::ElementsAre(LayerHasValidBitmapIs(true)));
  const uint64_t generationAfterFirst = rowsAfterFirst.front().generation;
  const uint32_t rasterizeCountAfterFirst = rowsAfterFirst.front().rasterizeCount;

  const auto inspectorTiles = compositor.snapshotCompositeTiles();
  auto layerTile = std::find_if(inspectorTiles.begin(), inspectorTiles.end(), [](const auto& tile) {
    return tile.kind == CompositorController::CompositeTileSnapshot::Kind::Layer;
  });
  ASSERT_NE(layerTile, inspectorTiles.end());
  EXPECT_TRUE(layerTile->hasValidBitmap);
  EXPECT_EQ(layerTile->bitmapDims, Vector2i(32, 32));
  EXPECT_NE(layerTile->textureSnapshot, nullptr)
      << "Geode layer diagnostics should keep the GPU texture snapshot so the layer panel can "
         "render a thumbnail without CPU readback.";
  EXPECT_TRUE(layerTile->thumbnailPixels.empty())
      << "Texture-backed diagnostics should not synthesize a CPU thumbnail.";

  const auto countersBeforeDrag = compositor.fastPathCountersForTesting();
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(5.0, 0.0));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rowsAfterDrag = compositor.snapshotLayerInspectorRows();
  EXPECT_THAT(rowsAfterDrag, ::testing::ElementsAre(
                                 ::testing::AllOf(LayerGenerationIs(generationAfterFirst),
                                                  LayerRasterizeCountIs(rasterizeCountAfterFirst))))
      << "Pure-translation drag must not mint a new texture generation.";

  const auto countersAfterDrag = compositor.fastPathCountersForTesting();
  EXPECT_EQ(countersAfterDrag.fastPathFrames, countersBeforeDrag.fastPathFrames + 1u);
  EXPECT_EQ(countersAfterDrag.slowPathFramesWithDirty, countersBeforeDrag.slowPathFramesWithDirty);
}

TEST_F(CompositorControllerTest, TextureBackedStaticSegmentsExposeDimensionsWithoutCpuBitmap) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <pattern id="p" x="0" y="0" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect width="4" height="4" fill="blue" />
      </pattern>
    </defs>
    <rect id="patterned" x="2" y="2" width="8" height="8" fill="url(#p)" />
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForTextureCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto segmentRows = compositor.snapshotSegmentInspectorRows();
  const auto segmentRowIt = std::find_if(segmentRows.begin(), segmentRows.end(),
                                         [](const auto& row) { return row.hasValidBitmap; });
  ASSERT_NE(segmentRowIt, segmentRows.end());
  EXPECT_EQ(segmentRowIt->bitmapSize, Vector2i(32, 32));

  const auto allTiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  const auto segmentTileIt = std::find_if(allTiles.begin(), allTiles.end(), [](const auto& tile) {
    return tile.layerEntity == entt::null && tile.textureSnapshot != nullptr;
  });
  ASSERT_NE(segmentTileIt, allTiles.end());
  EXPECT_TRUE(segmentTileIt->bitmap.empty());
  EXPECT_EQ(segmentTileIt->bitmapDims, Vector2i(32, 32));

  const auto metadataOnly =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
  const auto metadataSegmentIt =
      std::find_if(metadataOnly.begin(), metadataOnly.end(), [](const auto& tile) {
        return tile.layerEntity == entt::null && tile.bitmapDims == Vector2i(32, 32);
      });
  ASSERT_NE(metadataSegmentIt, metadataOnly.end());
  EXPECT_TRUE(metadataSegmentIt->bitmap.empty());
  EXPECT_EQ(metadataSegmentIt->textureSnapshot, nullptr);

  const auto inspectorTiles = compositor.snapshotCompositeTiles();
  const auto inspectorSegmentIt =
      std::find_if(inspectorTiles.begin(), inspectorTiles.end(), [](const auto& tile) {
        return tile.kind == CompositorController::CompositeTileSnapshot::Kind::Segment &&
               tile.textureSnapshot != nullptr;
      });
  ASSERT_NE(inspectorSegmentIt, inspectorTiles.end());
  EXPECT_TRUE(inspectorSegmentIt->hasValidBitmap);
  EXPECT_TRUE(inspectorSegmentIt->thumbnailPixels.empty());
  EXPECT_EQ(inspectorSegmentIt->bitmapDims, Vector2i(32, 32));
}

TEST_F(CompositorControllerTest, PromotedGroupWithMandatoryChildDragReusesTextureFastPath) {
  SVGDocument document = makeDocument(R"svg(
    <g id="Blue_center_burst">
      <ellipse id="burst_child" cx="45" cy="30" rx="18" ry="21" fill="blue" opacity="0.75" />
    </g>
  )svg");

  configureMockForTextureCaching();
  auto target = document.querySelector("#Blue_center_burst");
  auto child = document.querySelector("#burst_child");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(child.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();
  const Entity childEntity = child->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));

  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  const auto rowsAfterFirst = compositor.snapshotLayerInspectorRows();
  ASSERT_GT(rowsAfterFirst.size(), 1u)
      << "The opacity child should remain a mandatory promoted descendant layer.";
  const auto rowAfterFirst =
      std::find_if(rowsAfterFirst.begin(), rowsAfterFirst.end(),
                   [entity](const auto& row) { return row.entity == entity; });
  const auto childRowAfterFirst =
      std::find_if(rowsAfterFirst.begin(), rowsAfterFirst.end(),
                   [childEntity](const auto& row) { return row.entity == childEntity; });
  ASSERT_NE(rowAfterFirst, rowsAfterFirst.end());
  ASSERT_NE(childRowAfterFirst, rowsAfterFirst.end());
  ASSERT_TRUE(rowAfterFirst->hasValidBitmap);
  ASSERT_TRUE(childRowAfterFirst->hasValidBitmap);
  const uint64_t generationAfterFirst = rowAfterFirst->generation;
  const uint32_t rasterizeCountAfterFirst = rowAfterFirst->rasterizeCount;
  const uint64_t childGenerationAfterFirst = childRowAfterFirst->generation;
  const uint32_t childRasterizeCountAfterFirst = childRowAfterFirst->rasterizeCount;

  const auto countersBeforeDrag = compositor.fastPathCountersForTesting();
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(7.0, 11.0));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rowsAfterDrag = compositor.snapshotLayerInspectorRows();
  const auto rowAfterDrag =
      std::find_if(rowsAfterDrag.begin(), rowsAfterDrag.end(),
                   [entity](const auto& row) { return row.entity == entity; });
  const auto childRowAfterDrag =
      std::find_if(rowsAfterDrag.begin(), rowsAfterDrag.end(),
                   [childEntity](const auto& row) { return row.entity == childEntity; });
  ASSERT_NE(rowAfterDrag, rowsAfterDrag.end());
  ASSERT_NE(childRowAfterDrag, rowsAfterDrag.end());
  EXPECT_EQ(rowAfterDrag->generation, generationAfterFirst)
      << "A translation-only drag of a promoted group with a mandatory child layer must reuse the "
         "cached texture.";
  EXPECT_EQ(rowAfterDrag->rasterizeCount, rasterizeCountAfterFirst)
      << "Re-rasterizing this subtree on every drag frame is the #Blue_center_burst lag.";
  EXPECT_EQ(childRowAfterDrag->generation, childGenerationAfterFirst);
  EXPECT_EQ(childRowAfterDrag->rasterizeCount, childRasterizeCountAfterFirst);

  const auto countersAfterDrag = compositor.fastPathCountersForTesting();
  EXPECT_EQ(countersAfterDrag.fastPathFrames, countersBeforeDrag.fastPathFrames + 1u);
  EXPECT_EQ(countersAfterDrag.slowPathFramesWithDirty, countersBeforeDrag.slowPathFramesWithDirty);

  const auto* layer = compositor.findLayerForTest(entity);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(layer->canvasFromBitmap().isTranslation());
  EXPECT_NEAR(layer->canvasFromBitmap().translation().x, 7.0, 1e-10);
  EXPECT_NEAR(layer->canvasFromBitmap().translation().y, 11.0, 1e-10);

  const auto* childLayer = compositor.findLayerForTest(childEntity);
  ASSERT_NE(childLayer, nullptr);
  ASSERT_TRUE(childLayer->canvasFromBitmap().isTranslation());
  EXPECT_NEAR(childLayer->canvasFromBitmap().translation().x, 7.0, 1e-10);
  EXPECT_NEAR(childLayer->canvasFromBitmap().translation().y, 11.0, 1e-10);
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsEmitsThumbnailWithMaxSideClamp) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="0" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rows = compositor.snapshotLayerInspectorRows();
  ASSERT_EQ(rows.size(), 1u);
  const auto& row = rows.front();
  ASSERT_TRUE(row.hasValidBitmap);

  // Thumbnail must be non-empty, longer side clamped to `kLayerThumbnailMaxSide`,
  // and the RGBA8 byte buffer must match the dimensions.
  EXPECT_GT(row.thumbnailDims.x, 0);
  EXPECT_GT(row.thumbnailDims.y, 0);
  EXPECT_LE(std::max(row.thumbnailDims.x, row.thumbnailDims.y),
            CompositorController::kLayerThumbnailMaxSide);
  EXPECT_EQ(row.thumbnailPixels.size(), static_cast<size_t>(row.thumbnailDims.x) *
                                            static_cast<size_t>(row.thumbnailDims.y) * 4u);
}

TEST_F(CompositorControllerTest, SnapshotSegmentInspectorRowsEmitsOneRowPerSlot) {
  // With N promoted layers there should be N+1 segment slots (pre-first,
  // between-each-pair, post-last). Each populated slot reports
  // dimensions, generation, and a wall-clock for the most recent
  // rasterize.
  SVGDocument document = makeDocument(R"svg(
    <rect id="a" x="0" y="0" width="10" height="10" fill="blue" />
    <rect id="b" x="20" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto a = document.querySelector("#a");
  ASSERT_TRUE(a.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(a->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rows = compositor.snapshotSegmentInspectorRows();
  // One promoted layer → two segment slots (pre-layer + post-layer).
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_THAT(
      rows,
      ::testing::ElementsAre(
          ::testing::Field("slotIndex", &CompositorController::SegmentInspectorRow::slotIndex, 0u),
          ::testing::Field("slotIndex", &CompositorController::SegmentInspectorRow::slotIndex,
                           1u)));
  for (const auto& row : rows) {
    if (row.hasValidBitmap) {
      EXPECT_NE(row.bitmapSize, Vector2i::Zero());
      EXPECT_GT(row.generation, 0u);
    }
  }
}

TEST_F(CompositorControllerTest, CheapStaticSpanPlanChoosesImmediate) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="cheap" x="2" y="2" width="8" height="8" fill="blue" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _, StaticSpanPlanAtSlot(
                 1u, ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate),
                 ::testing::Field("visible", &StaticSpanPlan::visible, true),
                 ::testing::Field("hasExpensiveEffect", &StaticSpanPlan::hasExpensiveEffect, false),
                 ::testing::Field("estimatedDrawOps", &StaticSpanPlan::estimatedDrawOps, 1),
                 ::testing::Field("estimatedPathVerbs", &StaticSpanPlan::estimatedPathVerbs,
                                  ::testing::Gt(0)),
                 ::testing::Field("estimatedRetainedBytes", &StaticSpanPlan::estimatedRetainedBytes,
                                  ::testing::Gt(0u)),
                 EstimatedRedrawCostNoMoreThanCacheOverhead(),
                 ::testing::Field("staticHeuristicImmediate",
                                  &StaticSpanPlan::staticHeuristicImmediate, true),
                 ::testing::Field("dynamicHeuristicImmediate",
                                  &StaticSpanPlan::dynamicHeuristicImmediate, false),
                 ::testing::Field("spanRangeLabel", &StaticSpanPlan::spanRangeLabel,
                                  HasSubstr("rect#cheap")))));

  const auto immediateTiles =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::ImmediateOnly);
  const auto immediateTileIt =
      std::find_if(immediateTiles.begin(), immediateTiles.end(),
                   [](const CompositorTile& tile) { return tile.immediate; });
  ASSERT_NE(immediateTileIt, immediateTiles.end());
  EXPECT_FALSE(immediateTileIt->bitmap.empty());

  const auto inspectorTiles = compositor.snapshotCompositeTiles();
  const auto inspectorTileIt =
      std::find_if(inspectorTiles.begin(), inspectorTiles.end(), [](const auto& tile) {
        return tile.kind == CompositorController::CompositeTileSnapshot::Kind::Segment &&
               tile.immediate;
      });
  ASSERT_NE(inspectorTileIt, inspectorTiles.end());
  EXPECT_TRUE(inspectorTileIt->staticHeuristicImmediate);
  EXPECT_FALSE(inspectorTileIt->dynamicHeuristicImmediate);
  EXPECT_THAT(inspectorTileIt->spanRangeLabel, HasSubstr("rect#cheap"));
}

TEST_F(CompositorControllerTest, ImmediateStaticSpanComposesDirectlyIntoCurrentFrame) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="cheap" x="2" y="2" width="8" height="8" fill="blue" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EXPECT_CALL(renderer_, beginFrame(_)).Times(1);
  EXPECT_CALL(renderer_, endFrame()).Times(1);
  EXPECT_CALL(renderer_, drawPath(_, _)).Times(AtLeast(1));

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  EXPECT_THAT(plans, ::testing::ElementsAre(
                         _, StaticSpanPlanAtSlot(1u, ::testing::Field("mode", &StaticSpanPlan::mode,
                                                                      StaticSpanMode::Immediate))));
}

TEST_F(CompositorControllerTest, SmallGradientStaticSpanPlanCanChooseImmediate) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <linearGradient id="g" x1="0" y1="0" x2="8" y2="0" gradientUnits="userSpaceOnUse">
        <stop offset="0" stop-color="blue" />
        <stop offset="1" stop-color="white" />
      </linearGradient>
    </defs>
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="gradient" x="2" y="2" width="8" height="8" fill="url(#g)" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  // A gradient fill is not hard-blocked from immediate: its per-pixel cost is
  // folded into the rasterize estimate as an area term. This 8x8 gradient covers
  // a tiny area, so the estimate stays well under budget and it goes immediate —
  // a large gradient (e.g. a full-width cloud band) would estimate over budget
  // and stay cached instead.
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _,
          StaticSpanPlanAtSlot(
              1u, ::testing::Field("visible", &StaticSpanPlan::visible, true),
              ::testing::Field("hasExpensiveEffect", &StaticSpanPlan::hasExpensiveEffect, false),
              ::testing::Field("estimatedUsesAreaCostlyPaint",
                               &StaticSpanPlan::estimatedUsesAreaCostlyPaint, true),
              ::testing::Field("estimatedDrawOps", &StaticSpanPlan::estimatedDrawOps, 1),
              EstimatedRasterizeWithinImmediateBudget(),
              ::testing::AnyOf(::testing::Field("staticHeuristicImmediate",
                                                &StaticSpanPlan::staticHeuristicImmediate, true),
                               ::testing::Field("dynamicHeuristicImmediate",
                                                &StaticSpanPlan::dynamicHeuristicImmediate, true)),
              ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate),
              ::testing::Field("spanRangeLabel", &StaticSpanPlan::spanRangeLabel,
                               HasSubstr("rect#gradient")))))
      << "Gradient fills are direct path paints; the area-aware estimate decides immediate vs "
         "cached.";
}

TEST_F(CompositorControllerTest, StaticImmediateHeuristicDoesNotDemoteAfterSlowMeasurement) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="cheap" x="2" y="2" width="8" height="8" fill="blue" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  compositor.setStaticSpanRasterizeElapsedMsForTesting(5.0);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _, StaticSpanPlanAtSlot(
                 1u,
                 ::testing::Field("staticHeuristicImmediate",
                                  &StaticSpanPlan::staticHeuristicImmediate, true),
                 MeasuredRasterizeExceedsImmediateBudget(),
                 ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate))))
      << "Slow measured timing must not demote the static cheapness heuristic.";
}

TEST_F(CompositorControllerTest, CheapStaticSpanExpandsToImmediateByEstimate) {
  SVGDocument document = makeDocument(
      R"svg(
        <rect id="target" x="300" y="0" width="10" height="10" fill="red" />
        <rect id="medium" x="20" y="20" width="80" height="80" fill="blue" />
      )svg",
      Vector2i(512, 512));

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  compositor.setStaticSpanRasterizeElapsedMsForTesting(0.5);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{Vector2i(512, 512)});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  // The medium solid rect is too large for the cache-vs-redraw `staticHeuristic`,
  // but its geometry (one draw, a handful of verbs) estimates well under the
  // immediate budget, so the deterministic estimate expands it to immediate.
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _, StaticSpanPlanAtSlot(
                 1u,
                 ::testing::Field("staticHeuristicImmediate",
                                  &StaticSpanPlan::staticHeuristicImmediate, false),
                 EstimatedRasterizeWithinImmediateBudget(),
                 ::testing::Field("dynamicHeuristicImmediate",
                                  &StaticSpanPlan::dynamicHeuristicImmediate, true),
                 ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate))));

  const auto stats = compositor.lastRenderFrameStats();
  EXPECT_GE(stats.immediateTileCount, 1);
  EXPECT_GE(stats.cachedTileCount, 1);
}

TEST_F(CompositorControllerTest, DynamicImmediateSpanStaysImmediateDespiteSlowMeasuredRender) {
  SVGDocument document = makeDocument(
      R"svg(
        <rect id="target" x="300" y="0" width="10" height="10" fill="red" />
        <rect id="medium" x="20" y="20" width="80" height="80" fill="blue" />
      )svg",
      Vector2i(512, 512));

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  compositor.setStaticSpanRasterizeElapsedMsForTesting(0.5);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{Vector2i(512, 512)});

  {
    const auto plans = compositor.snapshotStaticSpanPlansForTesting();
    ASSERT_THAT(
        plans,
        ::testing::ElementsAre(
            _, StaticSpanPlanAtSlot(
                   1u,
                   ::testing::Field("staticHeuristicImmediate",
                                    &StaticSpanPlan::staticHeuristicImmediate, false),
                   ::testing::Field("dynamicHeuristicImmediate",
                                    &StaticSpanPlan::dynamicHeuristicImmediate, true),
                   ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate))));
  }

  // A later frame whose *measured* render is far over budget must NOT demote the
  // span: the immediate decision is driven by the deterministic geometry estimate
  // (`EstimateStaticSpanRasterizeMs`), which is unchanged because the span's
  // geometry is unchanged. This is the whole point of removing the measured-time
  // path — presentation no longer flaps with how slow one render happened to be.
  configureMockForCaching();
  compositor.setStaticSpanRasterizeElapsedMsForTesting(5.0);
  compositor.renderFrame(RenderViewport{Vector2i(512, 512)});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _,
          StaticSpanPlanAtSlot(
              1u,
              ::testing::Field("staticHeuristicImmediate",
                               &StaticSpanPlan::staticHeuristicImmediate, false),
              ::testing::Field("dynamicHeuristicImmediate",
                               &StaticSpanPlan::dynamicHeuristicImmediate, true),
              ::testing::Field("demotedDynamicImmediate", &StaticSpanPlan::demotedDynamicImmediate,
                               false),
              MeasuredRasterizeExceedsImmediateBudget(), EstimatedRasterizeWithinImmediateBudget(),
              ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::Immediate))))
      << "The mock injected a slow render, but the geometry estimate stays under budget so the "
         "decision is unchanged.";

  const auto stats = compositor.lastRenderFrameStats();
  EXPECT_GE(stats.immediateTileCount, 1);
  EXPECT_EQ(stats.cachedTileCount, 0);

  const auto inspectorTiles = compositor.snapshotCompositeTiles();
  const auto inspectorTileIt =
      std::find_if(inspectorTiles.begin(), inspectorTiles.end(), [](const auto& tile) {
        return tile.kind == CompositorController::CompositeTileSnapshot::Kind::Segment &&
               tile.dynamicHeuristicImmediate;
      });
  ASSERT_NE(inspectorTileIt, inspectorTiles.end());
  EXPECT_TRUE(inspectorTileIt->immediate);
  EXPECT_FALSE(inspectorTileIt->demotedDynamicImmediate);
  EXPECT_THAT(inspectorTileIt->spanRangeLabel, HasSubstr("rect#medium"));
}

TEST_F(CompositorControllerTest, PaintResourceStaticSpanPlanChoosesCachedTile) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <pattern id="p" x="0" y="0" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect width="4" height="4" fill="blue" />
      </pattern>
    </defs>
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="patterned" x="2" y="2" width="8" height="8" fill="url(#p)" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto plans = compositor.snapshotStaticSpanPlansForTesting();
  EXPECT_THAT(
      plans,
      ::testing::ElementsAre(
          _, StaticSpanPlanAtSlot(
                 1u, ::testing::Field("mode", &StaticSpanPlan::mode, StaticSpanMode::CachedTile),
                 ::testing::Field("hasExpensiveEffect", &StaticSpanPlan::hasExpensiveEffect, true),
                 ::testing::Field("estimatedDrawOps", &StaticSpanPlan::estimatedDrawOps,
                                  ::testing::Ge(1)))));

  const auto immediateTiles =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::ImmediateOnly);
  EXPECT_TRUE(std::none_of(immediateTiles.begin(), immediateTiles.end(),
                           [](const CompositorTile& tile) { return tile.immediate; }));
}

TEST_F(CompositorControllerTest, SnapshotTilesForUploadAppliesPayloadPolicies) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <pattern id="p" x="0" y="0" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect width="4" height="4" fill="blue" />
      </pattern>
    </defs>
    <rect id="patterned" x="2" y="2" width="8" height="8" fill="url(#p)" />
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto allTiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  const auto findTargetTile = [entity](const std::vector<CompositorTile>& tiles) {
    return std::find_if(tiles.begin(), tiles.end(), [entity](const CompositorTile& tile) {
      return tile.layerEntity == entity;
    });
  };
  const auto findCachedSegmentTile = [](const std::vector<CompositorTile>& tiles) {
    return std::find_if(tiles.begin(), tiles.end(), [](const CompositorTile& tile) {
      return tile.layerEntity == entt::null && !tile.immediate &&
             tile.bitmapDims != Vector2i::Zero();
    });
  };

  const auto allTarget = findTargetTile(allTiles);
  const auto allCachedSegment = findCachedSegmentTile(allTiles);
  ASSERT_NE(allTarget, allTiles.end());
  ASSERT_NE(allCachedSegment, allTiles.end());
  ASSERT_TRUE(allTarget->isDragTarget);
  ASSERT_FALSE(allTarget->bitmap.empty());
  ASSERT_FALSE(allCachedSegment->bitmap.empty());

  const auto dragOnly =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::DragTargetOnly);
  const auto dragOnlyTarget = findTargetTile(dragOnly);
  const auto dragOnlyCachedSegment = findCachedSegmentTile(dragOnly);
  ASSERT_NE(dragOnlyTarget, dragOnly.end());
  ASSERT_NE(dragOnlyCachedSegment, dragOnly.end());
  EXPECT_FALSE(dragOnlyTarget->bitmap.empty());
  EXPECT_TRUE(dragOnlyCachedSegment->bitmap.empty());
  EXPECT_NE(dragOnlyCachedSegment->bitmapDims, Vector2i::Zero());

  const auto immediateOnly =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::ImmediateOnly);
  const auto immediateOnlyCachedSegment = findCachedSegmentTile(immediateOnly);
  ASSERT_NE(immediateOnlyCachedSegment, immediateOnly.end());
  EXPECT_TRUE(immediateOnlyCachedSegment->bitmap.empty());
  EXPECT_NE(immediateOnlyCachedSegment->bitmapDims, Vector2i::Zero());

  const auto immediateAndDrag =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::ImmediateAndDragTargetOnly);
  const auto immediateAndDragTarget = findTargetTile(immediateAndDrag);
  const auto immediateAndDragCachedSegment = findCachedSegmentTile(immediateAndDrag);
  ASSERT_NE(immediateAndDragTarget, immediateAndDrag.end());
  ASSERT_NE(immediateAndDragCachedSegment, immediateAndDrag.end());
  EXPECT_FALSE(immediateAndDragTarget->bitmap.empty());
  EXPECT_TRUE(immediateAndDragCachedSegment->bitmap.empty());

  const auto metadataOnly =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
  ASSERT_EQ(metadataOnly.size(), allTiles.size());
  for (const CompositorTile& tile : metadataOnly) {
    EXPECT_TRUE(tile.bitmap.empty());
    EXPECT_EQ(tile.textureSnapshot, nullptr);
  }
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsCanOmitThumbnails) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rows =
      compositor.snapshotLayerInspectorRows(CompositorController::SnapshotThumbnails::Omit);
  EXPECT_THAT(
      rows,
      ::testing::ElementsAre(::testing::AllOf(
          LayerHasValidBitmapIs(true), LayerBitmapSizeIs(::testing::Ne(Vector2i::Zero())),
          LayerThumbnailDimsIs(Vector2i::Zero()), LayerThumbnailPixelsAre(::testing::IsEmpty()))));
}

TEST_F(CompositorControllerTest, SetTightBoundedSegmentsEnabledMarksExistingSegmentsDirty) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <pattern id="p" x="0" y="0" width="4" height="4" patternUnits="userSpaceOnUse">
        <rect width="4" height="4" fill="blue" />
      </pattern>
    </defs>
    <rect id="target" x="40" y="0" width="10" height="10" fill="red" />
    <rect id="static" x="2" y="2" width="8" height="8" fill="url(#p)" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.tightBoundedSegmentsEnabled());
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity()));
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rowsBeforeToggle = compositor.snapshotSegmentInspectorRows();
  ASSERT_FALSE(rowsBeforeToggle.empty());
  EXPECT_TRUE(std::none_of(rowsBeforeToggle.begin(), rowsBeforeToggle.end(),
                           [](const auto& row) { return row.dirty; }));

  compositor.setTightBoundedSegmentsEnabled(false);
  EXPECT_FALSE(compositor.tightBoundedSegmentsEnabled());

  const auto rowsAfterToggle = compositor.snapshotSegmentInspectorRows();
  ASSERT_EQ(rowsAfterToggle.size(), rowsBeforeToggle.size());
  EXPECT_TRUE(std::all_of(rowsAfterToggle.begin(), rowsAfterToggle.end(),
                          [](const auto& row) { return row.dirty; }));

  compositor.setTightBoundedSegmentsEnabled(false);
  const auto rowsAfterIdempotentToggle = compositor.snapshotSegmentInspectorRows();
  ASSERT_EQ(rowsAfterIdempotentToggle.size(), rowsAfterToggle.size());
  EXPECT_TRUE(std::all_of(rowsAfterIdempotentToggle.begin(), rowsAfterIdempotentToggle.end(),
                          [](const auto& row) { return row.dirty; }));
}

TEST_F(CompositorControllerTest, CancelledRenderLeavesDirtyLayerForNextFrame) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));

  CancellationToken token;
  token.cancel();
  EXPECT_FALSE(compositor.renderFrame(RenderViewport{kTestSvgDefaultSize}, token));

  {
    const auto rows = compositor.snapshotLayerInspectorRows();
    EXPECT_THAT(rows, ::testing::ElementsAre(
                          ::testing::AllOf(LayerDirtyIs(true), LayerHasValidBitmapIs(false))));
  }

  token.reset();
  EXPECT_TRUE(compositor.renderFrame(RenderViewport{kTestSvgDefaultSize}, token));

  const auto rows = compositor.snapshotLayerInspectorRows();
  EXPECT_THAT(rows, ::testing::ElementsAre(
                        ::testing::AllOf(LayerDirtyIs(false), LayerHasValidBitmapIs(true))));
}

TEST_F(CompositorControllerTest, IntrinsicSizeMandatoryFilterLayerHasNonZeroCanvasOffset) {
  // Design doc 0033 §M2A: mandatory-detected filter layers rasterize
  // into an offscreen sized to their tight canvas bounds (with 1px AA
  // padding), not the full viewport. The MockRenderer's takeSnapshot
  // returns a stub 1x1 bitmap regardless of viewport, so we can't
  // inspect dimensions directly — instead pin that `canvasOffset` is
  // non-zero (the rasterize went through the tight-bound path, which
  // is the architectural invariant we care about). The real-renderer
  // pixel correctness is covered by the `CompositorGolden_tests`.
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <filter id="f"><feGaussianBlur stdDeviation="2"/></filter>
    </defs>
    <rect width="200" height="100" fill="white"/>
    <g id="target" filter="url(#f)">
      <circle cx="40" cy="40" r="10" fill="red"/>
    </g>
  )svg",
                                      Vector2i(200, 100));

  configureMockForCaching();
  CompositorController compositor(document, renderer_);
  compositor.renderFrame(RenderViewport{Vector2d(200, 100)});

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto* layer = compositor.findLayerForTest(target->unsafeEntityHandle().entity());
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(layer->hasValidBitmap());

  // The circle is centered at (40, 40), so the tight bound's top-left
  // should be in the upper-left quadrant of the canvas — well away
  // from origin.
  EXPECT_GT(layer->canvasOffset().x, 0.0)
      << "Expected tight-bound rasterize; canvasOffset.x = " << layer->canvasOffset().x;
  EXPECT_GT(layer->canvasOffset().y, 0.0)
      << "Expected tight-bound rasterize; canvasOffset.y = " << layer->canvasOffset().y;
}

TEST_F(CompositorControllerTest, EditorPromotedLayerAlsoUsesIntrinsicSize) {
  // Design doc 0033 §M2B: editor-promoted layers (drag target /
  // selection prewarm) go through the same tight-bound rasterize as
  // mandatory-detected layers. The editor reads the layer's
  // `canvasOffset` via `CompositedPreview` and blits the texture at
  // intrinsic dimensions instead of stretching to canvas.
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" x="50" y="30" width="20" height="20" fill="red"/>
  )svg",
                                      Vector2i(200, 100));

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

  CompositorController compositor(document, renderer_);
  ASSERT_TRUE(compositor.promoteEntity(entity));
  compositor.renderFrame(RenderViewport{Vector2d(200, 100)});

  const auto* layer = compositor.findLayerForTest(entity);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(layer->hasValidBitmap());

  // The rect spans canvas (50,30)..(70,50), so the tight-bound rasterize
  // sets a non-zero canvasOffset somewhere in the upper-left quadrant.
  EXPECT_GT(layer->canvasOffset().x, 0.0)
      << "Editor-promoted layers must use intrinsic size; canvasOffset.x = "
      << layer->canvasOffset().x;
  EXPECT_GT(layer->canvasOffset().y, 0.0);

  // `layerCanvasOffsetOf` is the editor's accessor for the same value
  // and must agree with `findLayerForTest`.
  EXPECT_EQ(compositor.layerCanvasOffsetOf(entity), layer->canvasOffset());
}

TEST_F(CompositorControllerTest, IntrinsicSizeLayerFastPathTranslationStillWorks) {
  // The fast-path delta math is independent of bitmap size. Verify
  // that a translation applied to a mandatory-filter parent group
  // produces the expected `canvasFromBitmap` translation on top of
  // the layer's stable canvasOffset.
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <filter id="f"><feGaussianBlur stdDeviation="1"/></filter>
    </defs>
    <g id="target" filter="url(#f)">
      <rect x="0" y="0" width="20" height="20" fill="red"/>
    </g>
  )svg",
                                      Vector2i(200, 100));

  configureMockForCaching();
  CompositorController compositor(document, renderer_);
  compositor.renderFrame(RenderViewport{Vector2d(200, 100)});

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // Rasterize-time: canvasFromBitmap is identity, canvasOffset is the
  // bitmap's intrinsic position.
  const auto* layerBefore = compositor.findLayerForTest(target->unsafeEntityHandle().entity());
  ASSERT_NE(layerBefore, nullptr);
  const Vector2d offsetAtRasterize = layerBefore->canvasOffset();

  // Translate the group by (5, 10) in DOM space.
  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(5.0, 10.0));
  compositor.renderFrame(RenderViewport{Vector2d(200, 100)});

  const auto* layerAfter = compositor.findLayerForTest(target->unsafeEntityHandle().entity());
  ASSERT_NE(layerAfter, nullptr);
  EXPECT_EQ(layerAfter->canvasOffset(), offsetAtRasterize)
      << "canvasOffset stays put — only canvasFromBitmap encodes the drag delta.";
  const Transform2d xform = layerAfter->canvasFromBitmap();
  EXPECT_TRUE(xform.isTranslation());
  EXPECT_NEAR(xform.translation().x, 5.0, 1e-10);
  EXPECT_NEAR(xform.translation().y, 10.0, 1e-10);
}

TEST_F(CompositorControllerTest, MandatoryFilterLayerSurvivesCanvasResize) {
  // Regression for the detector-ordering bug surfaced by design doc 0033
  // M1's layer inspector: `RenderingContext::invalidateRenderTree()`
  // (called by `SVGDocument::setCanvasSize`) clears every
  // `RenderingInstanceComponent`. If `mandatoryDetector_.reconcile`
  // runs against that empty RIC view BEFORE
  // `prepareDocumentForRendering` rebuilds it, the detector scores
  // zero candidates and `reconcileLayers` drops the filter layer.
  // The next render frame must keep the filter group promoted.
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <filter id="f"><feGaussianBlur stdDeviation="2"/></filter>
    </defs>
    <g id="target" filter="url(#f)">
      <rect x="0" y="0" width="10" height="10" fill="red" />
    </g>
  )svg");

  configureMockForCaching();
  CompositorController compositor(document, renderer_);

  // First render: mandatory detector picks up the filter group.
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});
  ASSERT_FALSE(compositor.snapshotLayerInspectorRows().empty())
      << "Sanity check: mandatory filter detection should have promoted #target on first frame.";

  // Simulate a canvas resize — same code path as the editor's pinch-zoom
  // / window-resize path. `setCanvasSize` calls
  // `invalidateRenderTree()` which wipes every RIC.
  document.setCanvasSize(kTestSvgDefaultSize.x * 2, kTestSvgDefaultSize.y * 2);
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize * 2});

  // The filter group must still be promoted after the resize-driven
  // render. Without the fix, the snapshot returns zero rows because
  // the detector ran before prepare and saw an empty RIC view.
  const auto rows = compositor.snapshotLayerInspectorRows();
  EXPECT_FALSE(rows.empty())
      << "Canvas resize dropped the mandatory filter layer (detector ran against empty RICs).";
  bool sawFilterAfterResize = false;
  for (const auto& row : rows) {
    if ((row.fallbackReasons & FallbackReason::Filter) != FallbackReason::None) {
      sawFilterAfterResize = true;
    }
  }
  EXPECT_TRUE(sawFilterAfterResize)
      << "Filter-bearing layer should still be promoted after canvas resize.";
}

TEST_F(CompositorControllerTest, SnapshotLayerInspectorRowsFormatsFallbackReasons) {
  SVGDocument document = makeDocument(R"svg(
    <defs>
      <filter id="f"><feGaussianBlur stdDeviation="2"/></filter>
    </defs>
    <g id="target" filter="url(#f)">
      <rect x="0" y="0" width="10" height="10" fill="red" />
    </g>
  )svg");

  configureMockForCaching();
  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());

  CompositorController compositor(document, renderer_);
  // Filter-bearing entities are auto-promoted by MandatoryHintDetector;
  // drive a frame to let it run.
  compositor.renderFrame(RenderViewport{kTestSvgDefaultSize});

  const auto rows = compositor.snapshotLayerInspectorRows();
  ASSERT_FALSE(rows.empty());

  // At least one row should have the Filter fallback flag set, with the
  // string representation matching the FallbackReasonToString output.
  bool sawFilter = false;
  for (const auto& row : rows) {
    if ((row.fallbackReasons & FallbackReason::Filter) != FallbackReason::None) {
      sawFilter = true;
      EXPECT_NE(row.fallbackReasonsText.find("Filter"), std::string::npos)
          << "fallbackReasonsText='" << row.fallbackReasonsText << "'";
    }
  }
  EXPECT_TRUE(sawFilter);
}

TEST_F(CompositorControllerTest, ResetAllLayersAllowsRepromotion) {
  SVGDocument document = makeDocument(R"svg(
    <rect id="target" width="10" height="10" fill="red" />
  )svg");

  auto target = document.querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const Entity entity = target->unsafeEntityHandle().entity();

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

TEST_F(CompositorControllerTest, BuildStructuralEntityRemapMapsEquivalentTrees) {
  SVGDocument oldDocument = makeDocument(R"svg(
    <g id="parent">
      <rect id="target" x="1" y="2" width="10" height="10" fill="red" />
      <circle cx="20" cy="20" r="5" />
    </g>
  )svg");
  SVGDocument newDocument = makeDocument(R"svg(
    <g id="parent">
      <rect id="target" x="3" y="4" width="20" height="20" fill="blue" />
      <circle cx="25" cy="25" r="6" />
    </g>
  )svg");

  auto oldParent = oldDocument.querySelector("#parent");
  auto oldTarget = oldDocument.querySelector("#target");
  auto newParent = newDocument.querySelector("#parent");
  auto newTarget = newDocument.querySelector("#target");
  ASSERT_TRUE(oldParent.has_value());
  ASSERT_TRUE(oldTarget.has_value());
  ASSERT_TRUE(newParent.has_value());
  ASSERT_TRUE(newTarget.has_value());

  const auto remap = BuildStructuralEntityRemap(oldDocument, newDocument);

  ASSERT_FALSE(remap.empty());
  EXPECT_EQ(remap.at(oldDocument.svgElement().unsafeEntityHandle().entity()),
            newDocument.svgElement().unsafeEntityHandle().entity());
  EXPECT_EQ(remap.at(oldParent->unsafeEntityHandle().entity()),
            newParent->unsafeEntityHandle().entity());
  EXPECT_EQ(remap.at(oldTarget->unsafeEntityHandle().entity()),
            newTarget->unsafeEntityHandle().entity());
}

TEST_F(CompositorControllerTest, BuildStructuralEntityRemapRejectsStructuralMismatches) {
  SVGDocument oldDocument = makeDocument(R"svg(
    <g id="parent">
      <rect id="target" width="10" height="10" fill="red" />
    </g>
  )svg");

  SVGDocument tagMismatch = makeDocument(R"svg(
    <g id="parent">
      <circle id="target" cx="5" cy="5" r="5" fill="red" />
    </g>
  )svg");
  SVGDocument idMismatch = makeDocument(R"svg(
    <g id="parent">
      <rect id="other" width="10" height="10" fill="red" />
    </g>
  )svg");
  SVGDocument childCountMismatch = makeDocument(R"svg(
    <g id="parent">
      <rect id="target" width="10" height="10" fill="red" />
      <rect id="extra" width="5" height="5" fill="blue" />
    </g>
  )svg");

  EXPECT_TRUE(BuildStructuralEntityRemap(oldDocument, tagMismatch).empty());
  EXPECT_TRUE(BuildStructuralEntityRemap(oldDocument, idMismatch).empty());
  EXPECT_TRUE(BuildStructuralEntityRemap(oldDocument, childCountMismatch).empty());
}

}  // namespace donner::svg::compositor
