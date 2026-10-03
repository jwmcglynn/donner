#include "donner/editor/RenderCoordinator.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>
#include <vector>

#include "donner/base/tests/RunfileGate.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tests/FramePresentationTestAccess.h"
#include "donner/editor/tests/RenderCoordinatorTestAccess.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// GPU-free orchestration tests install explicitly captured resource manifests. Actual texture
// import and presentation are covered by the GL cache and editor replay suites.

namespace donner::editor {

namespace {

using ::testing::IsEmpty;

// gtest cannot stream the untyped `entt::null_t` sentinel, so compare against a
// concrete `Entity`-typed null. This keeps EXPECT_EQ's self-diagnosing output
// (it prints the actual entity id vs. null) instead of forcing a bare
// EXPECT_TRUE(x == entt::null).
constexpr Entity kNullEntity = entt::null;

constexpr std::string_view kTwoRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

constexpr std::string_view kHiddenRectSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="visible" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="hidden" x="50" y="50" width="20" height="20" fill="blue"
               style="display:none"/>
       </svg>)";

constexpr std::string_view kMixedSelectionSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs id="defs"><rect id="in-defs" x="0" y="0" width="10" height="10"/></defs>
         <rect id="visible1" x="10" y="10" width="20" height="20" fill="red"/>
         <rect id="visible2" x="40" y="10" width="20" height="20" fill="green"/>
         <rect id="hidden" x="70" y="10" width="20" height="20" fill="blue"
               style="display:none"/>
       </svg>)";

// A non-graphics element (a bare `<defs>`) so the "selected element is not a
// graphics element" branches in the predicates are exercised.
constexpr std::string_view kDefsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs id="d1"><rect id="r1" x="0" y="0" width="10" height="10"/></defs>
       </svg>)";

constexpr std::string_view kInheritedClipSvg =
    R"CLIP(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs><clipPath id="clip"><rect id="clip-shape" x="5" y="5" width="60" height="60"/></clipPath></defs>
         <g clip-path="url(#clip)"><rect id="child" x="10" y="10" width="20" height="20"/></g>
       </svg>)CLIP";

ViewportState MakeViewport(EditorApp& app) {
  ViewportState viewport;
  auto viewBox = app.document().document().svgElement().viewBox();
  if (viewBox.has_value()) {
    viewport.documentViewBox = *viewBox;
  } else {
    viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);
  }
  viewport.devicePixelRatio = 1.0;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(100.0, 100.0);
  viewport.resetTo100Percent();
  return viewport;
}

svg::SVGElement QuerySelector(EditorApp& app, std::string_view selector) {
  auto element = app.document().document().querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "querySelector(" << selector << ") returned nullopt";
  return *element;
}

bool WriteClipGuideHeldFrame(const svg::RendererBitmap& bitmap, std::string_view filename) {
  const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (outputDir == nullptr || bitmap.empty()) {
    return false;
  }
  const std::filesystem::path path = std::filesystem::path(outputDir) / filename;
  return svg::RendererImageIO::writeRgbaPixelsToPngFile(path.string().c_str(), bitmap.pixels,
                                                        bitmap.dimensions.x, bitmap.dimensions.y,
                                                        bitmap.rowBytes / 4u);
}

SelectTool::ActiveDragPreview DragPreview(Entity entity, std::uint64_t generation,
                                          Vector2d translation = Vector2d::Zero(),
                                          Transform2d documentFromCachedDocument = Transform2d()) {
  return SelectTool::ActiveDragPreview{
      .entity = entity,
      .translation = translation,
      .documentFromCachedDocument = documentFromCachedDocument,
      .dragGeneration = generation,
  };
}

// ---------------------------------------------------------------------------
// Free-function presentation policies (pure, no editor state).
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorPolicyTest, NothingToPresentRetryPacesTheFailedRequestThenHoldsIt) {
  NothingToPresentRetry retry;
  const RenderAttemptIdentity failing{.documentGeneration = 1, .version = 2};
  RenderAttemptIdentity newVersion = failing;
  newVersion.version = 3;
  auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  EXPECT_TRUE(retry.mayPost(failing, now));

  for (const std::chrono::milliseconds delay : NothingToPresentRetry::kRetryDelays) {
    SCOPED_TRACE(::testing::Message() << "retry delay " << delay.count() << " ms");
    EXPECT_FALSE(retry.noteFailure(failing, now));
    EXPECT_FALSE(retry.mayPost(failing, now + delay - std::chrono::milliseconds(1)));
    EXPECT_TRUE(retry.mayPost(newVersion, now)) << "a different request is never held";
    EXPECT_THAT(retry.secondsUntilRetry(now),
                ::testing::Optional(
                    ::testing::FloatNear(std::chrono::duration<float>(delay).count(), 1e-4f)));
    now += delay;
    EXPECT_TRUE(retry.mayPost(failing, now));
    EXPECT_EQ(retry.secondsUntilRetry(now), std::nullopt)
        << "a due retry asks for no further wake; the next frame that renders posts it";
  }

  EXPECT_TRUE(retry.noteFailure(failing, now)) << "the failure after the last retry gives up";
  EXPECT_FALSE(retry.retryScheduled());
  EXPECT_EQ(retry.secondsUntilRetry(now), std::nullopt);
  EXPECT_FALSE(retry.mayPost(failing, now + std::chrono::hours(1)))
      << "an identical request waits until something about it changes";
  EXPECT_TRUE(retry.mayPost(newVersion, now));
  EXPECT_FALSE(retry.noteFailure(failing, now)) << "giving up is reported once";

  retry.reset();
  EXPECT_TRUE(retry.mayPost(failing, now));
}

TEST(RenderCoordinatorPolicyTest, NothingToPresentRetryTreatsAChangedRasterOrDragAsANewRequest) {
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  RenderAttemptIdentity failing{.documentGeneration = 1, .version = 2};
  failing.rasterViewport.outputSizePx = Vector2i(64, 64);
  failing.dragPreview = RenderRequest::DragPreview{.translation = Vector2d(4.0, 0.0)};
  NothingToPresentRetry retry;
  ASSERT_FALSE(retry.noteFailure(failing, now));
  ASSERT_FALSE(retry.mayPost(failing, now));

  RenderAttemptIdentity zoomed = failing;
  zoomed.rasterViewport.outputSizePx = Vector2i(128, 128);
  EXPECT_TRUE(retry.mayPost(zoomed, now));
  RenderAttemptIdentity dragged = failing;
  dragged.dragPreview->translation = Vector2d(6.0, 0.0);
  EXPECT_TRUE(retry.mayPost(dragged, now));
  RenderAttemptIdentity replaced = failing;
  replaced.documentGeneration = 2;
  EXPECT_TRUE(retry.mayPost(replaced, now));
  RenderAttemptIdentity refreshed = failing;
  refreshed.presentationEpoch = 1;
  EXPECT_TRUE(retry.mayPost(refreshed, now)) << "a presentation refresh is a new request";
  RenderAttemptIdentity recaptured = failing;
  recaptured.dragPreview->forceLayerRasterization = true;
  EXPECT_FALSE(retry.mayPost(recaptured, now))
      << "the recapture flag is a scheduler hint, not a different request";

  EXPECT_FALSE(retry.noteFailure(zoomed, now));
  EXPECT_TRUE(retry.mayPost(failing, now)) << "a new failure replaces the held request";
  EXPECT_FALSE(retry.mayPost(zoomed, now));
}

TEST(RenderCoordinatorPolicyTest, FullCanvasPreviewAlwaysPresentable) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile;
  tile.id = "full-canvas";
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.bitmap.dimensions = Vector2i(64, 64);
  tile.bitmap.rowBytes = 64u * 4u;
  tile.bitmap.pixels.resize(64u * 64u * 4u);
  preview.tiles.push_back(tile);
  ASSERT_TRUE(preview.valid());

  // A canvas mismatch is irrelevant for the single full-canvas tile: it may
  // stretch across transient canvas-size changes.
  EXPECT_TRUE(ShouldPresentCompositedPreviewForViewport(preview, Vector2i(999, 999)));
}

TEST(RenderCoordinatorPolicyTest, InvalidPreviewIsNeverPresentable) {
  RenderResult::CompositedPreview preview;  // No tiles → invalid.
  ASSERT_FALSE(preview.valid());
  EXPECT_FALSE(ShouldPresentCompositedPreviewForViewport(preview, Vector2i(64, 64)));
}

TEST(RenderCoordinatorPolicyTest, SplitPreviewPresentableOnlyWhenTileCanvasMatchesViewport) {
  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile;
  tile.id = "layer-1";
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.rasterCanvasSize = Vector2i(64, 64);
  tile.bitmap.dimensions = Vector2i(16, 16);
  tile.bitmap.rowBytes = 16u * 4u;
  tile.bitmap.pixels.resize(16u * 16u * 4u);
  preview.tiles.push_back(tile);
  ASSERT_TRUE(preview.valid());

  // Within the ±1 px tolerance: presentable.
  EXPECT_TRUE(ShouldPresentCompositedPreviewForViewport(preview, Vector2i(65, 63)));
  // Outside tolerance: a stale high-resolution tile would flash in the wrong
  // place, so the policy refuses it.
  EXPECT_FALSE(ShouldPresentCompositedPreviewForViewport(preview, Vector2i(128, 128)));
  // Degenerate viewport canvas is never presentable for a split preview.
  EXPECT_FALSE(ShouldPresentCompositedPreviewForViewport(preview, Vector2i(0, 0)));
}

TEST(RenderCoordinatorPolicyTest, ReleaseSettleWaitsForQueuedTransformFlush) {
  EXPECT_EQ(PostReleaseSettleTargetVersion(/*currentFrameVersion=*/42,
                                           /*hasPendingMutations=*/false),
            42u);
  EXPECT_EQ(PostReleaseSettleTargetVersion(/*currentFrameVersion=*/42,
                                           /*hasPendingMutations=*/true),
            43u)
      << "Mouse-up can queue the final transform while the renderer is busy. A stale in-flight "
         "render at the already-flushed version must not close the settle window before that "
         "queued transform is flushed.";
  EXPECT_EQ(PostReleaseSettleTargetVersion(std::numeric_limits<std::uint64_t>::max(),
                                           /*hasPendingMutations=*/true),
            std::numeric_limits<std::uint64_t>::max());
}

TEST(RenderCoordinatorPolicyTest, PendingSelectedLayerRasterizationBypassesViewportDefer) {
  const Entity selectedEntity = static_cast<Entity>(7);

  EXPECT_TRUE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/true))
      << "A style/fill edit marks the selected layer pixels stale; the coordinator must request "
         "the forced layer rasterization immediately instead of deferring forever on an unsettled "
         "viewport.";
}

TEST(RenderCoordinatorPolicyTest, SelectedViewportRefreshDeferRequiresEveryPredicate) {
  const Entity selectedEntity = static_cast<Entity>(7);

  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      entt::null, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/true, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/9, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/false, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/true,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedPresentation=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/true, /*pendingSelectedLayerRasterization=*/false));
}

TEST(RenderCoordinatorPolicyTest, SelectionPrewarmPreservesCompleteVisibleCoverageAtAnyPaneSize) {
  const Entity selectedEntity = static_cast<Entity>(7);

  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/false, /*hasCompleteVisibleCachedCoverage=*/true,
      Vector2i(800, 600), Vector2i(1200, 900)))
      << "Even a small pane rebuilds every cached static segment when selected prewarm changes "
         "the output dimensions.";
  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/false,
      /*hasIndependentRenderReason=*/false, /*hasCompleteVisibleCachedCoverage=*/false,
      Vector2i(800, 600), Vector2i(900, 650)))
      << "Selecting on a direct surface must not manufacture a raster-viewport change that posts "
         "an otherwise-identical worker frame.";
  EXPECT_TRUE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/false,
      /*hasIndependentRenderReason=*/true, /*hasCompleteVisibleCachedCoverage=*/false,
      Vector2i(800, 600), Vector2i(900, 650)))
      << "Without complete coverage, a modest incremental overdraw can accompany a real render.";

  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/true, /*hasCompleteVisibleCachedCoverage=*/true,
      Vector2i(800, 600), Vector2i(900, 650)))
      << "An active drag must retain complete visible background tiles on small panes too.";
  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/true, /*hasCompleteVisibleCachedCoverage=*/false,
      Vector2i(800, 600), Vector2i(1200, 900)))
      << "A cold pane should not multiply the first render area just to prewarm selection.";

  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/false, /*hasCompleteVisibleCachedCoverage=*/true,
      Vector2i(2774, 2048), Vector2i(3072, 2048)))
      << "Retina selection must retain the already-complete visible tile set instead of "
         "rebuilding full-scene tiles beyond the surface budget";
  EXPECT_FALSE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/true, /*hasCompleteVisibleCachedCoverage=*/true,
      Vector2i(2774, 2048), Vector2i(3072, 2048)))
      << "Active drag must not switch back to the oversized raster and invalidate its tiles";
  EXPECT_TRUE(ShouldUseSelectedPrewarmRasterViewport(
      selectedEntity, /*requestOverviewInfill=*/false, /*rasterViewportBounded=*/true,
      /*selectionOnlyPrewarmMayTriggerRender=*/true,
      /*hasIndependentRenderReason=*/true, /*hasCompleteVisibleCachedCoverage=*/true,
      Vector2i(2048, 1536), Vector2i(2048, 1536)))
      << "A full-document raster with unchanged dimensions has no extra tile cost";
}

TEST(RenderCoordinatorPolicyTest, OnlyForcedSelectedResultClearsPendingLayerRasterization) {
  const Entity selectedEntity = static_cast<Entity>(7);

  RenderRequest::DragPreview regularSelectionPreview;
  regularSelectionPreview.entity = selectedEntity;
  regularSelectionPreview.interactionKind = svg::compositor::InteractionHint::Selection;
  regularSelectionPreview.forceLayerRasterization = false;

  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      regularSelectionPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8))
      << "An already-running selected prewarm can complete at the same document version as a "
         "queued fill/style flush, but it still contains the old pixels unless the request carried "
         "forceLayerRasterization.";

  RenderRequest::DragPreview forcedSelectionPreview = regularSelectionPreview;
  forcedSelectionPreview.forceLayerRasterization = true;
  EXPECT_TRUE(ShouldClearPendingSelectedLayerRasterization(
      forcedSelectionPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedSelectionPreview, selectedEntity, /*resultVersion=*/7, /*pendingVersion=*/8));

  forcedSelectionPreview.entity = static_cast<Entity>(8);
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedSelectionPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));
}

TEST(RenderCoordinatorPolicyTest, PendingSelectedLayerClearRequiresEntityPreviewAndVersion) {
  const Entity selectedEntity = static_cast<Entity>(7);

  RenderRequest::DragPreview forcedPreview;
  forcedPreview.entity = selectedEntity;
  forcedPreview.forceLayerRasterization = true;

  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedPreview, entt::null, /*resultVersion=*/8, /*pendingVersion=*/8));
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedPreview, selectedEntity, /*resultVersion=*/7, /*pendingVersion=*/8));
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      std::nullopt, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));

  forcedPreview.entity = static_cast<Entity>(8);
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));

  forcedPreview.entity = selectedEntity;
  forcedPreview.forceLayerRasterization = false;
  EXPECT_FALSE(ShouldClearPendingSelectedLayerRasterization(
      forcedPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));
}

TEST(RenderCoordinatorPolicyTest, ForcedOwningTilesClearPendingSelectedRasterization) {
  const Entity selectedEntity = static_cast<Entity>(7);
  RenderRequest::DragPreview forcedPreview;
  forcedPreview.entity = selectedEntity;
  forcedPreview.interactionKind = svg::compositor::InteractionHint::Selection;
  forcedPreview.forceLayerRasterization = true;

  RenderResult::CompositedPreview owningPreview;
  owningPreview.tiles.emplace_back();
  owningPreview.tiles.front().id = "owning-span";
  owningPreview.entity = entt::null;
  owningPreview.representedDragPreview = forcedPreview;
  EXPECT_TRUE(CompositedPreviewClearsPendingSelectedLayerRasterization(
      owningPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8))
      << "A complete forced owning-tile frame refreshes unpromotable selected text/style";

  owningPreview.representedDragPreview->forceLayerRasterization = false;
  EXPECT_FALSE(CompositedPreviewClearsPendingSelectedLayerRasterization(
      owningPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));
  owningPreview.representedDragPreview->forceLayerRasterization = true;
  owningPreview.tiles.clear();
  EXPECT_FALSE(CompositedPreviewClearsPendingSelectedLayerRasterization(
      owningPreview, selectedEntity, /*resultVersion=*/8, /*pendingVersion=*/8));
}

TEST(RenderCoordinatorPolicyTest, RepresentedDragPreviewFollowsActiveTargetWhenPresentable) {
  const SelectTool::ActiveDragPreview active =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(5.0, 2.0));
  const SelectTool::ActiveDragPreview displayed =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(1.0, 1.0));

  EXPECT_EQ(OverlayRepresentedDragPreviewForPresentation(std::nullopt, displayed,
                                                         /*hasPresentableActiveDragTarget=*/true),
            std::nullopt);

  const std::optional<SelectTool::ActiveDragPreview> represented =
      OverlayRepresentedDragPreviewForPresentation(active, displayed,
                                                   /*hasPresentableActiveDragTarget=*/true);
  ASSERT_TRUE(represented.has_value());
  EXPECT_EQ(represented->translation, active.translation);
}

TEST(RenderCoordinatorPolicyTest, RepresentedDragPreviewReusesDisplayedMatchingGeneration) {
  const SelectTool::ActiveDragPreview active =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(5.0, 2.0));
  const SelectTool::ActiveDragPreview displayed =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(1.0, 1.0));

  const std::optional<SelectTool::ActiveDragPreview> represented =
      OverlayRepresentedDragPreviewForPresentation(active, displayed,
                                                   /*hasPresentableActiveDragTarget=*/false);
  ASSERT_TRUE(represented.has_value());
  EXPECT_EQ(represented->translation, displayed.translation);
}

TEST(RenderCoordinatorPolicyTest, RepresentedDragPreviewFallsBackForMismatchedDisplayedState) {
  const SelectTool::ActiveDragPreview active =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(5.0, 2.0));
  const SelectTool::ActiveDragPreview displayed =
      DragPreview(static_cast<Entity>(43), 9, Vector2d(1.0, 1.0));

  const std::optional<SelectTool::ActiveDragPreview> represented =
      OverlayRepresentedDragPreviewForPresentation(active, displayed,
                                                   /*hasPresentableActiveDragTarget=*/false);
  ASSERT_TRUE(represented.has_value());
  EXPECT_EQ(represented->entity, active.entity);
  EXPECT_EQ(represented->translation, Vector2d::Zero());
  EXPECT_TRUE(represented->documentFromCachedDocument.isIdentity());
  EXPECT_EQ(represented->dragGeneration, active.dragGeneration);
}

TEST(RenderCoordinatorPolicyTest, RepresentedDragPreviewFallsBackForGenerationMismatch) {
  const SelectTool::ActiveDragPreview active =
      DragPreview(static_cast<Entity>(42), 7, Vector2d(5.0, 2.0));
  const SelectTool::ActiveDragPreview staleDisplayed =
      DragPreview(static_cast<Entity>(42), 6, Vector2d(1.0, 1.0));

  const std::optional<SelectTool::ActiveDragPreview> represented =
      OverlayRepresentedDragPreviewForPresentation(active, staleDisplayed,
                                                   /*hasPresentableActiveDragTarget=*/false);
  ASSERT_TRUE(represented.has_value());
  EXPECT_EQ(represented->entity, active.entity);
  EXPECT_EQ(represented->translation, Vector2d::Zero());
  EXPECT_EQ(represented->dragGeneration, active.dragGeneration);
}

TEST(RenderCoordinatorPolicyTest, RepresentedDocumentTransformRequiresMatchingInvertibleDrag) {
  const SelectTool::ActiveDragPreview live = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(10.0, 0.0), Transform2d::Translate(Vector2d(10.0, 0.0)));
  const SelectTool::ActiveDragPreview represented = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(3.0, 0.0), Transform2d::Translate(Vector2d(3.0, 0.0)));

  EXPECT_TRUE(OverlayDocumentFromSourceDragPreview(std::nullopt, represented).isIdentity());
  EXPECT_TRUE(OverlayDocumentFromSourceDragPreview(
                  live, DragPreview(static_cast<Entity>(42), 8, Vector2d(3.0, 0.0)))
                  .isIdentity());
  EXPECT_TRUE(
      OverlayDocumentFromSourceDragPreview(
          DragPreview(static_cast<Entity>(42), 7, Vector2d::Zero(), Transform2d::Scale(0.0)),
          represented)
          .isIdentity());

  const Transform2d projected = OverlayDocumentFromSourceDragPreview(live, represented);
  EXPECT_FALSE(projected.isIdentity());
  EXPECT_NE(projected.data[4], 0.0);
}

TEST(RenderCoordinatorPolicyTest, GesturePreviewProjectsOntoRepresentedDragState) {
  SelectTool::ActiveGesturePreview gesture;
  gesture.kind = SelectTool::ActiveGestureKind::Move;
  gesture.startBoundsDoc = Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0);
  gesture.documentFromStartDocument = Transform2d::Translate(Vector2d(10.0, 0.0));
  gesture.currentDocumentDelta = Vector2d(10.0, 0.0);

  EXPECT_EQ(OverlayGesturePreviewForPresentation(std::nullopt, std::nullopt, std::nullopt),
            std::nullopt);

  const SelectTool::ActiveDragPreview live = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(10.0, 0.0), Transform2d::Translate(Vector2d(10.0, 0.0)));
  const SelectTool::ActiveDragPreview represented = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(3.0, 0.0), Transform2d::Translate(Vector2d(3.0, 0.0)));
  const std::optional<SelectTool::ActiveGesturePreview> projected =
      OverlayGesturePreviewForPresentation(gesture, live, represented);
  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(projected->currentDocumentDelta, represented.translation);
  EXPECT_FALSE(projected->documentFromStartDocument.isIdentity());
}

TEST(RenderCoordinatorPolicyTest, GesturePreviewKeepsLiveDeltaForUnmatchedRepresentedDrag) {
  SelectTool::ActiveGesturePreview gesture;
  gesture.kind = SelectTool::ActiveGestureKind::Move;
  gesture.startBoundsDoc = Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0);
  gesture.documentFromStartDocument = Transform2d::Translate(Vector2d(10.0, 0.0));
  gesture.currentDocumentDelta = Vector2d(10.0, 0.0);

  const SelectTool::ActiveDragPreview live = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(10.0, 0.0), Transform2d::Translate(Vector2d(10.0, 0.0)));
  const SelectTool::ActiveDragPreview represented = DragPreview(
      static_cast<Entity>(43), 7, Vector2d(3.0, 0.0), Transform2d::Translate(Vector2d(3.0, 0.0)));

  const std::optional<SelectTool::ActiveGesturePreview> projected =
      OverlayGesturePreviewForPresentation(gesture, live, represented);
  ASSERT_TRUE(projected.has_value());
  EXPECT_EQ(projected->currentDocumentDelta, gesture.currentDocumentDelta);
  EXPECT_EQ(projected->documentFromStartDocument.data[4],
            gesture.documentFromStartDocument.data[4]);
  EXPECT_EQ(projected->documentFromStartDocument.data[5],
            gesture.documentFromStartDocument.data[5]);
}

// ---------------------------------------------------------------------------
// setSourceHoverElements - change detection.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, SetSourceHoverElementsReportsChange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement r1 = QuerySelector(app, "#r1");
  EXPECT_TRUE(coordinator.setSourceHoverElements({r1}))
      << "First non-empty hover set must report a change.";
  EXPECT_FALSE(coordinator.setSourceHoverElements({r1}))
      << "Re-setting the identical hover set must report no change.";

  const svg::SVGElement r2 = QuerySelector(app, "#r2");
  EXPECT_TRUE(coordinator.setSourceHoverElements({r2}))
      << "A different hover element must report a change.";
  EXPECT_TRUE(coordinator.setSourceHoverElements({}))
      << "Clearing a non-empty hover set must report a change.";
  EXPECT_FALSE(coordinator.setSourceHoverElements({}))
      << "Clearing an already-empty hover set must report no change.";
}

// ---------------------------------------------------------------------------
// selectedElementIsDisplayNone - predicate over the live selection.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, SelectedElementIsDisplayNoneFalseWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;
  EXPECT_FALSE(coordinator.selectedElementIsDisplayNone(app));
}

TEST(RenderCoordinatorTest, SelectedElementIsDisplayNoneFalseForVisibleSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  app.setSelection(QuerySelector(app, "#visible"));
  EXPECT_FALSE(coordinator.selectedElementIsDisplayNone(app));
}

TEST(RenderCoordinatorTest, SelectedElementIsDisplayNoneTrueForHiddenSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  app.setSelection(QuerySelector(app, "#hidden"));
  EXPECT_TRUE(coordinator.selectedElementIsDisplayNone(app));
}

TEST(RenderCoordinatorTest, SelectedElementIsDisplayNoneFalseForNonGraphicsSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kDefsSvg));
  RenderCoordinator coordinator;

  // `<defs>` is not an `SVGGraphicsElement`, so it is never treated as a
  // display:none graphics selection.
  app.setSelection(QuerySelector(app, "#d1"));
  EXPECT_FALSE(coordinator.selectedElementIsDisplayNone(app));
}

TEST(RenderCoordinatorTest, SelectedCompositedEntityDiagnosticsSkipsDisplayNoneAndNonGraphics) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kMixedSelectionSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement visible1 = QuerySelector(app, "#visible1");
  const svg::SVGElement visible2 = QuerySelector(app, "#visible2");
  const svg::SVGElement hidden = QuerySelector(app, "#hidden");
  const svg::SVGElement defs = QuerySelector(app, "#defs");
  const Entity visible1Entity = visible1.unsafeEntityHandle().entity();

  app.setSelection(hidden);
  EXPECT_EQ(coordinator.selectedCompositedEntityForDiagnostics(app), kNullEntity);

  app.setSelection({visible1, visible2, visible1, hidden, defs});
  EXPECT_EQ(coordinator.selectedCompositedEntityForDiagnostics(app), visible1Entity);
}

// ---------------------------------------------------------------------------
// suppressedCompositedLayerEntity - display:none stale-layer suppression.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, SuppressedLayerEntityNullWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), kNullEntity);
}

TEST(RenderCoordinatorTest, SuppressionSnapshotDoesNotWaitForDocumentWriter) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  RenderCoordinator coordinator;
  app.setSelection(QuerySelector(app, "#hidden"));
  const Entity expected = coordinator.suppressedCompositedLayerEntity(app);
  ASSERT_NE(expected, kNullEntity);

  std::promise<void> writerReady;
  std::promise<void> releaseWriter;
  auto release = releaseWriter.get_future();
  auto writer = std::async(std::launch::async, [&] {
    auto access = app.document().document().writeAccess();
    writerReady.set_value();
    release.wait();
  });
  writerReady.get_future().wait();
  auto suppressed = std::async(std::launch::async,
                               [&] { return coordinator.suppressedCompositedLayerEntity(app); });
  const auto status = suppressed.wait_for(std::chrono::milliseconds(100));
  releaseWriter.set_value();
  writer.get();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(suppressed.get(), expected);
}

TEST(RenderCoordinatorTest, VisibilitySnapshotDoesNotWaitForDocumentWriter) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  RenderCoordinator coordinator;
  app.setSelection(QuerySelector(app, "#hidden"));
  ASSERT_NE(coordinator.suppressedCompositedLayerEntity(app), kNullEntity);
  std::promise<void> writerReady;
  std::promise<void> releaseWriter;
  auto release = releaseWriter.get_future();
  auto writer = std::async(std::launch::async, [&] {
    auto access = app.document().document().writeAccess();
    writerReady.set_value();
    release.wait();
  });
  writerReady.get_future().wait();
  auto hidden =
      std::async(std::launch::async, [&] { return coordinator.selectedElementIsDisplayNone(app); });
  const auto status = hidden.wait_for(std::chrono::milliseconds(100));
  releaseWriter.set_value();
  writer.get();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_TRUE(hidden.get());
}

TEST(RenderCoordinatorTest, SuppressedLayerEntityNullForVisibleSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  app.setSelection(QuerySelector(app, "#visible"));
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), kNullEntity);
}

TEST(RenderCoordinatorTest, SuppressedLayerEntityFallsBackToSelfWhenNoCachedTextures) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement hidden = QuerySelector(app, "#hidden");
  const Entity hiddenEntity = hidden.unsafeEntityHandle().entity();
  app.setSelection(hidden);

  // With no composited cache, the live selected display:none entity is its own
  // suppression target - there is no separately-cached promoted layer to hide.
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), hiddenEntity);

  // The selection is sticky: a second query returns the same suppression
  // target without a cache.
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), hiddenEntity);
}

TEST(RenderCoordinatorTest, SuppressedLayerEntityPrefersCachedPromotedLayer) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement hidden = QuerySelector(app, "#hidden");
  const Entity hiddenEntity = hidden.unsafeEntityHandle().entity();

  // Seed a cached promoted layer for some prior entity (use the visible rect's
  // entity as a stand-in for a previously-promoted layer).
  const Entity cachedEntity = QuerySelector(app, "#visible").unsafeEntityHandle().entity();
  coordinator.compositedPresentation().noteCachedTextures(cachedEntity, /*version=*/1,
                                                          Vector2i(100, 100));
  ASSERT_TRUE(coordinator.compositedPresentation().diagnostics().hasCachedTextures);

  app.setSelection(hidden);
  // The cached promoted layer is the entity whose stale pixels must be hidden
  // while the display:none element's chrome stays up.
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), cachedEntity);
  EXPECT_NE(cachedEntity, hiddenEntity);
}

TEST(RenderCoordinatorTest, SuppressedLayerEntityClearsWhenSelectionBecomesVisible) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement hidden = QuerySelector(app, "#hidden");
  const Entity hiddenEntity = hidden.unsafeEntityHandle().entity();
  app.setSelection(hidden);
  ASSERT_EQ(coordinator.suppressedCompositedLayerEntity(app), hiddenEntity);

  // Selecting the visible rect (which is not the suppressed entity) clears the
  // suppression and returns null.
  app.setSelection(QuerySelector(app, "#visible"));
  // The visible rect is neither the suppressed selection nor layer, so the
  // stale suppression persists until its own layer is observed gone. The
  // contract here is simply that a visible selection is never *itself*
  // suppressed.
  EXPECT_NE(coordinator.suppressedCompositedLayerEntity(app),
            QuerySelector(app, "#visible").unsafeEntityHandle().entity());
}

// ---------------------------------------------------------------------------
// Selection-bounds cache: refresh + promote.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, RefreshSelectionBoundsCacheEmptyWithoutDocument) {
  EditorApp app;
  RenderCoordinator coordinator;
  ASSERT_FALSE(app.hasDocument());

  coordinator.refreshSelectionBoundsCache(app);
  EXPECT_THAT(coordinator.selectionBoundsCache().lastSelection, IsEmpty());
  EXPECT_THAT(coordinator.selectionBoundsCache().displayedBoundsDoc, IsEmpty());
}

TEST(RenderCoordinatorTest, RefreshSelectionBoundsCacheCapturesSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;

  const svg::SVGElement r1 = QuerySelector(app, "#r1");
  app.setSelection(r1);

  coordinator.refreshSelectionBoundsCache(app);
  const SelectionBoundsCache& cache = coordinator.selectionBoundsCache();
  ASSERT_EQ(cache.lastSelection.size(), 1u);
  EXPECT_TRUE(cache.lastSelection.front() == r1);
  EXPECT_EQ(cache.lastRefreshVersion, app.document().currentFrameVersion());
  // The single selected rect has renderable geometry → one pending bound.
  ASSERT_EQ(cache.pendingBoundsDoc.size(), 1u);
  // r1 lives at (10,10..30,30) in document space.
  EXPECT_THAT(cache.pendingBoundsDoc.front().topLeft.x, ::testing::DoubleNear(10.0, 1e-6));
  EXPECT_THAT(cache.pendingBoundsDoc.front().topLeft.y, ::testing::DoubleNear(10.0, 1e-6));
}

TEST(RenderCoordinatorTest, PromoteSelectionBoundsNoOpUntilDisplayedVersionCatchesUp) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;

  app.setSelection(QuerySelector(app, "#r1"));
  coordinator.refreshSelectionBoundsCache(app);

  // displayedDocVersion_ is still 0 (no async result polled), while the live
  // document version is >= 1, so the pending bounds cannot promote yet.
  ASSERT_GT(app.document().currentFrameVersion(), 0u);
  ASSERT_EQ(coordinator.displayedDocVersion(), 0u);
  ASSERT_FALSE(coordinator.selectionBoundsCache().pendingBoundsDoc.empty());

  coordinator.promoteSelectionBoundsIfReady();
  EXPECT_THAT(coordinator.selectionBoundsCache().displayedBoundsDoc, IsEmpty())
      << "Pending bounds must not promote while displayedDocVersion lags the pending version.";
  EXPECT_FALSE(coordinator.selectionBoundsCache().pendingBoundsDoc.empty())
      << "Unpromoted pending bounds must be retained for a later catch-up.";
}

// ---------------------------------------------------------------------------
// resetForLoadedDocument - clears all coordinator-owned state.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, ResetForLoadedDocumentClearsCachesAndOverlayState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;

  // Populate selection-bounds cache and composited cache.
  app.setSelection(QuerySelector(app, "#r1"));
  coordinator.refreshSelectionBoundsCache(app);
  coordinator.compositedPresentation().noteCachedTextures(
      QuerySelector(app, "#r1").unsafeEntityHandle().entity(), /*version=*/1, Vector2i(100, 100));
  coordinator.setSourceHoverElements({QuerySelector(app, "#r2")});

  ASSERT_FALSE(coordinator.selectionBoundsCache().lastSelection.empty());
  ASSERT_TRUE(coordinator.compositedPresentation().hasCachedTextures());

  coordinator.resetForLoadedDocument(app.document().documentGeneration());

  EXPECT_THAT(coordinator.selectionBoundsCache().lastSelection, IsEmpty());
  EXPECT_FALSE(coordinator.compositedPresentation().hasCachedTextures());
  EXPECT_EQ(coordinator.displayedDocVersion(), 0u);
  EXPECT_FALSE(coordinator.immediateOverlaySnapshot().has_value());

  // A hover set re-issued after reset reports a change (the cleared set differs
  // from the new one) - confirming the hover state was actually cleared.
  EXPECT_TRUE(coordinator.setSourceHoverElements({QuerySelector(app, "#r2")}));
}

// ---------------------------------------------------------------------------
// rasterizeOverlayForCurrentSelection - GL-free immediate overlay snapshotting.
// ---------------------------------------------------------------------------

std::shared_ptr<const CapturedPresentation> AcceptCapturedScene(
    RenderCoordinator& coordinator, EditorApp& app, std::span<const svg::SVGElement> hover = {},
    const std::optional<LockedRejectionFlashInput>& flash = std::nullopt) {
  std::vector<Entity> independent;
  for (const auto& element : app.selectedElements()) {
    independent.push_back(element.unsafeEntityHandle().entity());
  }
  const auto capture = CapturedPresentation::Capture(
      app.document().document(),
      PresentationIdentity{.captureId = 1,
                           .documentGeneration = app.document().documentGeneration(),
                           .version = app.document().currentFrameVersion(),
                           .geometryRevision = app.document().nonTransformRevision(),
                           .fontResourceRevision = app.document().fontResourceRevision()},
      app.selectedElements(), {}, independent, hover, flash);
  EXPECT_NE(capture, nullptr);
  std::vector<GlTextureCache::TileView> tiles;
  for (const auto& object : capture->objects()) {
    GlTextureCache::TileView tile;
    tile.texture = 1;
    tile.kind = RenderResult::CompositedTile::Kind::Layer;
    tile.layerEntity = object.entity;
    tile.canvasOffsetDoc = -capture->documentOrigin();
    tile.bitmapDimsDoc = Vector2d(2048.0, 2048.0);
    tile.bitmapDimsPx = Vector2i(2048, 2048);
    tiles.push_back(tile);
  }
  const auto resources = FramePresentationTestAccess::resources(capture, std::move(tiles));
  coordinator.compositedPresentation().notePreparedResources(
      resources, independent.empty() ? Entity(entt::null) : independent.front(), std::nullopt);
  return capture;
}

std::shared_ptr<const FramePresentation> BuildFrame(
    RenderCoordinator& coordinator, EditorApp& app, SelectTool& tool, const ViewportState& viewport,
    GlTextureCache& textures, SelectionChromeDetail detail = SelectionChromeDetail::Full) {
  return coordinator.buildFramePresentation(
      app, tool, viewport, Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize),
      detail);
}

TEST(RenderCoordinatorTest, FrameRequiresAcceptedRasterResources) {
  EditorApp app;
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  ViewportState viewport;
  EXPECT_EQ(BuildFrame(coordinator, app, tool, viewport, textures), nullptr);
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  viewport = MakeViewport(app);
  coordinator.requestSelectionGeometryRefresh();
  EXPECT_EQ(BuildFrame(coordinator, app, tool, viewport, textures), nullptr);
  EXPECT_FALSE(coordinator.immediateOverlaySnapshot().has_value());
  AcceptCapturedScene(coordinator, app);
  const auto frame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(frame, nullptr);
  EXPECT_THAT(frame->chrome().paths, ::testing::SizeIs(1));
}

TEST(RenderCoordinatorTest, RendererRefusalRetainsCompleteFrameAndChromeUntilRecovery) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  auto viewport = MakeViewport(app);
  app.setSelection(QuerySelector(app, "#r1"));
  AcceptCapturedScene(coordinator, app);
  const auto oldFrame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(oldFrame, nullptr);
  viewport.panBy(Vector2d(20, 0));
  int attempts = 0;
  const auto retained = coordinator.buildFramePresentation(
      app, tool, viewport, Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize),
      SelectionChromeDetail::Full, true, [&](const FramePresentation& candidate) {
        ++attempts;
        EXPECT_NE(candidate.frameId(), oldFrame->frameId());
        EXPECT_EQ(candidate.viewport().panScreenPoint, viewport.panScreenPoint);
        return false;
      });
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(retained, oldFrame);
  EXPECT_EQ(coordinator.framePresentation(), oldFrame);
  EXPECT_EQ(coordinator.immediateOverlaySnapshot()->canvasFromDoc.data[4],
            oldFrame->chrome().canvasFromDoc.data[4]);
  EXPECT_FALSE(coordinator.frameRepresentsCurrentIntent());
  const auto recovered = coordinator.buildFramePresentation(
      app, tool, viewport, Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize),
      SelectionChromeDetail::Full, true, [](const FramePresentation&) { return true; });
  ASSERT_NE(recovered, nullptr);
  EXPECT_NE(recovered, oldFrame);
  EXPECT_EQ(recovered->viewport().panScreenPoint, viewport.panScreenPoint);
  EXPECT_TRUE(coordinator.frameRepresentsCurrentIntent());
}

TEST(RenderCoordinatorTest, BusyDragProjectsCapturedFrameWithoutDocumentAccess) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  AcceptCapturedScene(coordinator, app);
  ASSERT_NE(BuildFrame(coordinator, app, tool, viewport, textures), nullptr);
  tool.onMouseMove(app, Vector2d(25.0, 15.0), true);
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  std::promise<void> writerReady;
  std::promise<void> releaseWriter;
  auto release = releaseWriter.get_future();
  auto writer = std::async(std::launch::async, [&] {
    auto access = app.document().document().writeAccess();
    writerReady.set_value();
    release.wait();
  });
  writerReady.get_future().wait();
  auto presentation = std::async(
      std::launch::async, [&] { return BuildFrame(coordinator, app, tool, viewport, textures); });
  const auto status = presentation.wait_for(std::chrono::milliseconds(100));
  releaseWriter.set_value();
  writer.get();
  EXPECT_EQ(status, std::future_status::ready);
  const auto frame = presentation.get();
  ASSERT_NE(frame, nullptr);
  ASSERT_THAT(frame->chrome().paths, ::testing::SizeIs(1));
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(),
            Box2d::FromXYWH(20.0, 10.0, 20.0, 20.0));
  EXPECT_EQ(frame->tiles().front().documentFromCachedDocument.translation(), Vector2d(10.0, 0.0));
}

TEST(RenderCoordinatorTest, SecondDragKeepsStaleRasterAndChromeAtTheSamePose) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(35.0, 15.0), true);
  ASSERT_TRUE(app.flushFrame());
  AcceptCapturedScene(coordinator, app);
  tool.onMouseMove(app, Vector2d(65.0, 15.0), true);
  ASSERT_TRUE(app.flushFrame());
  coordinator.compositedPresentation().beginSettling(tool.activeDragPreview(),
                                                     app.document().currentFrameVersion());
  tool.onMouseUp(app, Vector2d(65.0, 15.0));
  tool.onMouseDown(app, Vector2d(65.0, 15.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(70.0, 15.0), true);
  const auto frame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(frame, nullptr);
  ASSERT_THAT(frame->chrome().paths, ::testing::SizeIs(1));
  const Box2d outline = frame->chrome().paths.front().pathDoc.bounds();
  EXPECT_EQ(outline, Box2d::FromXYWH(65.0, 10.0, 20.0, 20.0));
  const auto& raster = frame->tiles().front();
  const Box2d paintedRect =
      raster.documentFromCachedDocument.transformBox(Box2d::FromXYWH(30.0, 10.0, 20.0, 20.0));
  EXPECT_EQ(paintedRect, outline);
}

TEST(RenderCoordinatorTest, SelectionRecaptureRequiresActualDocumentRevision) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  app.setSelection(QuerySelector(app, "#r1"));
  const auto source = AcceptCapturedScene(coordinator, app);
  const auto oldVersion = app.document().currentFrameVersion();
  auto second = QuerySelector(app, "#r2");
  second.setAttribute("x", "60");
  ASSERT_EQ(app.document().currentFrameVersion(), oldVersion);
  ASSERT_NE(app.document().document().handle()->revision(), source->identity().documentRevision);
  app.setSelection(second);
  coordinator.requestSelectionGeometryRefresh();
  const auto staleFrame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(staleFrame, nullptr);
  EXPECT_FALSE(staleFrame->hasSelectionGeometry());
  EXPECT_THAT(staleFrame->chrome().paths, IsEmpty());
  AcceptCapturedScene(coordinator, app);
  const auto freshFrame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(freshFrame, nullptr);
  ASSERT_THAT(freshFrame->chrome().paths, ::testing::SizeIs(1));
  EXPECT_EQ(freshFrame->chrome().paths.front().pathDoc.bounds(),
            Box2d::FromXYWH(60.0, 50.0, 20.0, 20.0));
}

TEST(RenderCoordinatorTest, SelectionRecaptureBindsToTheOverviewChosenForThisCamera) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  const PresentationIdentity identity{.captureId = 1,
                                      .documentGeneration = app.document().documentGeneration(),
                                      .version = app.document().currentFrameVersion(),
                                      .geometryRevision = app.document().nonTransformRevision()};
  const auto overview = CapturedPresentation::Capture(app.document().document(), identity, {});
  app.setSelection(QuerySelector(app, "#r1"));
  auto selectedIdentity = identity;
  selectedIdentity.captureId = 2;
  const auto active = CapturedPresentation::Capture(app.document().document(), selectedIdentity,
                                                    app.selectedElements());
  ASSERT_EQ(active->identity().documentRevision, overview->identity().documentRevision);
  GlTextureCache::TileView tile;
  tile.texture = 1;
  tile.bitmapDimsDoc = Vector2d(100, 100);
  tile.bitmapDimsPx = Vector2i(100, 100);
  const auto resources = FramePresentationTestAccess::resources(
      active, {tile},
      PresentationCoverageDiagnostics{.activeTilesViewportBounded = true,
                                      .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 50, 50)},
      overview, {tile});
  coordinator.compositedPresentation().notePreparedResources(resources, active->selection().front(),
                                                             std::nullopt);
  const auto frame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->identity().captureId, overview->identity().captureId);
  EXPECT_TRUE(frame->hasSelectionGeometry());
  ASSERT_THAT(frame->chrome().paths, ::testing::SizeIs(1));
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(), Box2d::FromXYWH(10, 10, 20, 20));
}

TEST(RenderCoordinatorTest, SameSceneSelectionChangeRecapturesWithoutNewRaster) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  app.setSelection(QuerySelector(app, "#r1"));
  const auto source = AcceptCapturedScene(coordinator, app);
  app.setSelection(QuerySelector(app, "#r2"));
  const auto frame = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->identity(), source->identity());
  ASSERT_THAT(frame->chrome().paths, ::testing::SizeIs(1));
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(),
            Box2d::FromXYWH(50.0, 50.0, 20.0, 20.0));
}

TEST(RenderCoordinatorTest, InheritedClipStaysWithFrozenRasterUntilReplacement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInheritedClipSvg));
  svg::Renderer renderer;
  renderer.draw(app.document().document());
  app.setSelection(QuerySelector(app, "#child"));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto viewport = MakeViewport(app);
  AcceptCapturedScene(coordinator, app);
  const auto original = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(original, nullptr);
  ASSERT_THAT(original->chrome().clipGuidesDoc, ::testing::SizeIs(1));
  const auto guide = original->chrome().clipGuidesDoc.front().pathDoc;
  app.applyMutation(
      EditorCommand::SetAttributeCommand(QuerySelector(app, "#clip-shape"), "x", "8"));
  ASSERT_TRUE(app.flushFrame());
  coordinator.requestSelectionGeometryRefresh();
  const auto stale = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(stale, nullptr);
  ASSERT_THAT(stale->chrome().clipGuidesDoc, ::testing::SizeIs(1));
  EXPECT_EQ(stale->chrome().clipGuidesDoc.front().pathDoc, guide);
  renderer.draw(app.document().document());
  AcceptCapturedScene(coordinator, app);
  const auto fresh = BuildFrame(coordinator, app, tool, viewport, textures);
  ASSERT_NE(fresh, nullptr);
  ASSERT_THAT(fresh->chrome().clipGuidesDoc, ::testing::SizeIs(1));
  EXPECT_NE(fresh->chrome().clipGuidesDoc.front().pathDoc, guide);
}

TEST(RenderCoordinatorTest, GeodeCrownClipGuideRendersOnFirstAndSecondHeldMove) {
  const auto source = ::donner::tests::ReadRequiredRunfile("geode_splash.svg");
  ASSERT_TRUE(source.ok()) << source.error;
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source.contents));
  app.document().document().setCanvasSize(1536, 1024);
  svg::Renderer prepared;
  prepared.draw(app.document().document());
  const svg::SVGElement crown = QuerySelector(app, "#central-crown-face-2");
  app.setSelection(crown);
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  ViewportState viewport = MakeViewport(app);
  viewport.paneSize = Vector2d(1536.0, 1024.0);
  viewport.resetTo100Percent();
  AcceptCapturedScene(coordinator, app);
  ASSERT_TRUE(BuildFrame(coordinator, app, tool, viewport, textures));
  ASSERT_TRUE(coordinator.immediateOverlaySnapshot().has_value());
  ASSERT_EQ(coordinator.immediateOverlaySnapshot()->clipGuidesDoc.size(), 1u);
  const Path inheritedClip = coordinator.immediateOverlaySnapshot()->clipGuidesDoc.front().pathDoc;
  const std::uint64_t clipRevision = app.document().nonTransformRevision();

  SelectTool::ActiveTransformBoundsPreview boundsPreview;
  boundsPreview.startBoundsDoc = Box2d::FromXYWH(518.0, 481.0, 38.0, 34.0);
  std::optional<svg::RendererBitmap> firstHeld;
  for (int x : {12, 20}) {
    const Transform2d movement = Transform2d::Translate(static_cast<double>(x), 0.0);
    app.applyMutation(EditorCommand::SetTransformCommand(crown, movement));
    ASSERT_TRUE(app.document().flushFrame());
    EXPECT_EQ(app.document().nonTransformRevision(), clipRevision);
    prepared.draw(app.document().document());
    boundsPreview.documentFromStartDocument = movement;
    const auto preview =
        DragPreview(crown.unsafeEntityHandle().entity(), 17, Vector2d(x, 0.0), movement);
    AcceptCapturedScene(coordinator, app);
    ASSERT_TRUE(BuildFrame(coordinator, app, tool, viewport, textures));
    ASSERT_TRUE(coordinator.immediateOverlaySnapshot().has_value());
    const SelectionChromeSnapshot withGuide = *coordinator.immediateOverlaySnapshot();
    ASSERT_EQ(withGuide.clipGuidesDoc.size(), 1u) << "held x=" << x;
    EXPECT_EQ(withGuide.clipGuidesDoc.front().pathDoc, inheritedClip);
    SelectionChromeSnapshot withoutGuide = withGuide;
    withoutGuide.clipGuidesDoc.clear();

    svg::Renderer withRenderer;
    withRenderer.draw(app.document().document());
    withRenderer.setPreserveTargetOnBeginFrame(true);
    svg::RenderViewport rasterViewport;
    rasterViewport.size = Vector2d(1536.0, 1024.0);
    rasterViewport.devicePixelRatio = 1.0;
    withRenderer.beginFrame(rasterViewport);
    OverlayRenderer::drawChromeFromSnapshot(withRenderer, withGuide);
    withRenderer.endFrame();
    const svg::RendererBitmap withBitmap = withRenderer.takeSnapshot();
    svg::Renderer withoutRenderer;
    withoutRenderer.draw(app.document().document());
    withoutRenderer.setPreserveTargetOnBeginFrame(true);
    withoutRenderer.beginFrame(rasterViewport);
    OverlayRenderer::drawChromeFromSnapshot(withoutRenderer, withoutGuide);
    withoutRenderer.endFrame();
    const svg::RendererBitmap withoutBitmap = withoutRenderer.takeSnapshot();
    int guidePixels = 0;
    tests::CompareBitmapToBitmap(
        withBitmap, withoutBitmap, "geode_crown_held_clip_guide_vs_control",
        tests::ApprovedPixelToleranceParams(0.0f, std::numeric_limits<int>::max(), true),
        &guidePixels);
    EXPECT_GT(guidePixels, 20) << "Guide must remain visible while mouse is held at x=" << x;
    const std::string_view filename =
        x == 12 ? "geode_crown_held_clip_guide_12.png" : "geode_crown_held_clip_guide_20.png";
    if (std::getenv("TEST_UNDECLARED_OUTPUTS_DIR") != nullptr) {
      EXPECT_TRUE(WriteClipGuideHeldFrame(withBitmap, filename));
    }
    if (firstHeld.has_value()) {
      int movedPixels = 0;
      tests::CompareBitmapToBitmap(
          withBitmap, *firstHeld, "geode_crown_first_vs_second_held",
          tests::ApprovedPixelToleranceParams(0.0f, std::numeric_limits<int>::max(), true),
          &movedPixels);
      EXPECT_GT(movedPixels, 20) << "Artwork and selection must advance between held positions";
    } else {
      firstHeld = withBitmap;
    }
  }
}

// ---------------------------------------------------------------------------
// maybeRequestRender - GL-free orchestration that posts an async render request.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, MaybeRequestRenderNoOpWithoutDocument) {
  EditorApp app;
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  ViewportState viewport;
  viewport.paneSize = Vector2d(100.0, 100.0);

  // No document → early return, no async render dispatched.
  coordinator.maybeRequestRender(app, selectTool, viewport, &textures);
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy());
}

TEST(RenderCoordinatorTest, MaybeRequestRenderNoOpWithDegeneratePane) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  ViewportState viewport;  // paneSize is zero.

  coordinator.maybeRequestRender(app, selectTool, viewport, &textures);
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy());
}

TEST(RenderCoordinatorTest, MaybeRequestRenderDispatchesAsyncRenderForSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  ViewportState viewport = MakeViewport(app);

  app.setSelection(QuerySelector(app, "#r1"));

  coordinator.maybeRequestRender(app, selectTool, viewport, &textures);

  // A render request was posted to the async worker. We do NOT poll the result
  // here: presenting a composited preview calls GlTextureCache::uploadComposited
  // (raw GL), which is unreachable without a GL context. Cancel the in-flight
  // render and drain to idle so the worker thread joins cleanly at teardown.
  coordinator.asyncRenderer().cancelInFlight();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (coordinator.asyncRenderer().isBusy() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy())
      << "cancelInFlight must return the worker to idle.";

  // Committing the canvas size is part of maybeRequestRender's contract; it set
  // the live document canvas to the viewport's desired size.
  EXPECT_EQ(app.document().document().canvasSize(), viewport.desiredCanvasSize());
}

TEST(RenderCoordinatorTest, MaybeRequestRenderDispatchesWithoutTextureCache) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  SelectTool selectTool;
  ViewportState viewport = MakeViewport(app);

  app.setSelection(QuerySelector(app, "#r1"));

  coordinator.maybeRequestRender(app, selectTool, viewport, /*textures=*/nullptr);
  coordinator.asyncRenderer().cancelInFlight();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (coordinator.asyncRenderer().isBusy() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy());
  EXPECT_EQ(app.document().document().canvasSize(), viewport.desiredCanvasSize());
}

TEST(RenderCoordinatorTest, HeldDragWithoutPromotedTileRendersChangedDocumentVersion) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  app.document().document().setCanvasSize(100, 100);
  const svg::SVGElement target = QuerySelector(app, "#r1");
  app.setSelection(target);
  const Entity selectedEntity = target.unsafeEntityHandle().entity();
  const std::uint64_t representedVersion = app.document().currentFrameVersion();
  const EditorRasterViewport rasterViewport = viewport.rasterViewport();
  RenderCoordinatorTestAccess::noteRenderCompleted(coordinator, representedVersion, rasterViewport);

  selectTool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  selectTool.onMouseMove(app, Vector2d(35.0, 15.0), /*buttonHeld=*/true);
  ASSERT_TRUE(selectTool.activeDragPreview().has_value());
  ASSERT_TRUE(app.flushFrame());
  ASSERT_GT(app.document().currentFrameVersion(), representedVersion);
  // The worker described the selection, but a masked/owning layer did not yield a tile that
  // could be translated on the UI thread. Model that exact metadata/pixel mismatch here.
  coordinator.compositedPresentation().noteCachedTextures(selectedEntity, representedVersion,
                                                          rasterViewport.outputSizePx,
                                                          selectTool.activeDragPreview());
  ASSERT_TRUE(textures.tiles().empty());

  EXPECT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, &textures))
      << "A live drag with no presentable target tile must render while the pointer is held";
  coordinator.asyncRenderer().cancelInFlight();
  EXPECT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

TEST(RenderCoordinatorTest, CancelledPixelCaptureRepostsWithoutDocumentOrViewportChange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  app.setSelection(QuerySelector(app, "#r1"));
  coordinator.asyncRenderer().setReplayRenderDelayForTesting(std::chrono::milliseconds(100));
  coordinator.setDocumentPixelCaptureEnabled(true);
  ASSERT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, &textures));

  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  ASSERT_FALSE(coordinator.asyncRenderer().isBusy());
  coordinator.pollRenderResult(app, viewport, textures);
  EXPECT_TRUE(coordinator.presentationRefreshPending());
  EXPECT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, &textures))
      << "A dropped worker result must not leave a same-epoch picker permanently pending.";

  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy());
}

TEST(RenderCoordinatorTest, SelectedPixelCaptureRepostsAfterPanCancelsInFlightResult) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  ViewportState viewport = MakeViewport(app);
  app.setSelection(QuerySelector(app, "#r1"));
  coordinator.asyncRenderer().setReplayRenderDelayForTesting(std::chrono::milliseconds(100));
  coordinator.setDocumentPixelCaptureEnabled(true);
  ASSERT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, &textures));

  viewport.panScreenPoint.x += 20.0;
  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  ASSERT_FALSE(coordinator.asyncRenderer().isBusy());
  coordinator.pollRenderResult(app, viewport, textures);
  EXPECT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, &textures));

  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy());
}

TEST(RenderCoordinatorTest, DelayedCanvasSizeCommitInvalidatesSameVersionPixelCapture) {
  constexpr std::string_view kPercentSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100%" height="100%">
           <rect id="percent" width="50%" height="50%" fill="red"/></svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPercentSvg));
  app.document().document().setCanvasSize(100, 100);
  const std::uint64_t versionBefore = app.document().currentFrameVersion();
  ViewportState viewport = MakeViewport(app);
  viewport.zoom = 2.0;
  RenderCoordinator coordinator;
  SelectTool selectTool;
  coordinator.asyncRenderer().setReplayRenderDelayForTesting(std::chrono::milliseconds(500));
  RenderCoordinatorTestAccess::seedPreCommitPixelCapture(coordinator, app, viewport);
  ASSERT_NE(coordinator.documentPixelCaptureFor(app, viewport), nullptr);

  ASSERT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, /*textures=*/nullptr));
  EXPECT_EQ(coordinator.documentPixelCaptureFor(app, viewport), nullptr)
      << "A raster made before the semantic canvas commit cannot become Ready during debounce.";
  EXPECT_FALSE(coordinator.nextPixelCaptureCanvasCommitWakeSeconds().has_value())
      << "Worker completion wakes the shell; no timer should spin while it is busy.";

  RenderCoordinatorTestAccess::makeCanvasCommitDue(coordinator);
  ASSERT_TRUE(coordinator.asyncRenderer().isBusy());
  EXPECT_FALSE(coordinator.maybeRequestRender(app, selectTool, viewport, /*textures=*/nullptr));
  EXPECT_EQ(coordinator.documentPixelCaptureFor(app, viewport), nullptr)
      << "An elapsed deadline cannot expose the stale raster while the worker delays commit.";
  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  EXPECT_THAT(coordinator.nextPixelCaptureCanvasCommitWakeSeconds(),
              ::testing::Optional(::testing::Eq(0.0f)));
  coordinator.maybeRequestRender(app, selectTool, viewport, /*textures=*/nullptr);
  EXPECT_EQ(coordinator.documentCanvasCommitTotal(), 1u);
  EXPECT_EQ(app.document().currentFrameVersion(), versionBefore);
  EXPECT_EQ(coordinator.documentPixelCaptureFor(app, viewport), nullptr);
  EXPECT_THAT(RenderCoordinatorTestAccess::requestedCommitGeneration(coordinator),
              ::testing::Optional(1u))
      << "The post-commit request must name the new layout even when frame version is unchanged.";

  coordinator.asyncRenderer().cancelInFlight();
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

// ---------------------------------------------------------------------------
// A worker result that carries nothing to present.
// ---------------------------------------------------------------------------

/// Runs one editor frame's render handoff the way the shell does: consume the finished worker
/// result, then ask for the next render at the end of the frame. Returns true when a request was
/// posted. A result with nothing to present is rejected before any texture upload, so this runs
/// without a GPU context as long as every result is withheld.
bool RunRenderFrame(RenderCoordinator& coordinator, EditorApp& app, SelectTool& selectTool,
                    const ViewportState& viewport, GlTextureCache& textures) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (coordinator.asyncRenderer().isBusy() && std::chrono::steady_clock::now() < deadline) {
    coordinator.pollRenderResult(app, viewport, textures);
    if (coordinator.asyncRenderer().isBusy()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  EXPECT_FALSE(coordinator.asyncRenderer().isBusy()) << "the worker never finished its render";
  return coordinator.maybeRequestRender(app, selectTool, viewport, &textures);
}

int CountPostedRenders(RenderCoordinator& coordinator, EditorApp& app, SelectTool& selectTool,
                       const ViewportState& viewport, GlTextureCache& textures, int frames) {
  int posted = 0;
  for (int frame = 0; frame < frames; ++frame) {
    if (RunRenderFrame(coordinator, app, selectTool, viewport, textures)) {
      ++posted;
    }
  }
  return posted;
}

// Every idle frame asks the coordinator for a render, and a result with nothing to present marks
// neither its version nor its raster rendered. Without pacing, a failure that persists (a lost
// device, surfaces the renderer keeps refusing) re-posts the identical failing request on every
// frame, and every result wakes the next frame: the worker and the UI loop spin at full rate.
TEST(RenderCoordinatorTest, RenderWithNothingToPresentIsNotRepostedEveryFrame) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);

  const int posted = CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10);
  EXPECT_THAT(posted, ::testing::AllOf(::testing::Ge(1), ::testing::Le(2)))
      << "ten idle frames of a render that keeps producing nothing to present";
  EXPECT_EQ(posted, 1) << "the first retry waits for its delay";
  EXPECT_EQ(coordinator.displayedDocVersionForDiagnostics(), 0u)
      << "a result with nothing to present must not become the displayed version";
  EXPECT_THAT(coordinator.nextNothingToPresentRetryWakeSeconds(),
              ::testing::Optional(::testing::FloatNear(0.1f, 1e-4f)))
      << "the idle loop must wake for the retry without input";

  for (const std::chrono::milliseconds delay : NothingToPresentRetry::kRetryDelays) {
    RenderCoordinatorTestAccess::advanceFakeRetryClock(delay);
    EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1)
        << "one retry once " << delay.count() << " ms have passed";
  }
  RenderCoordinatorTestAccess::advanceFakeRetryClock(std::chrono::minutes(1));
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 0)
      << "after the last retry fails, the identical request waits until something changes";
  EXPECT_EQ(coordinator.nextNothingToPresentRetryWakeSeconds(), std::nullopt);
  EXPECT_EQ(coordinator.nothingToPresentResultTotalForDiagnostics(),
            NothingToPresentRetry::kRetryDelays.size() + 1);
  EXPECT_EQ(coordinator.displayedDocVersionForDiagnostics(), 0u);
}

TEST(RenderCoordinatorTest, FailedSelectedOverdrawRetriesVisibleRasterAfterRetryBudgetExhausts) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000"
         viewBox="0 0 1000 1000">
      <rect id="target" x="100" y="100" width="100" height="100" fill="red"/>
      <rect id="peer" x="300" y="100" width="100" height="100" fill="blue"/>
    </svg>
  )svg"));
  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());
  app.setSelection(*target);

  ViewportState viewport;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 1000.0, 1000.0);
  viewport.paneSize = Vector2d(200.0, 120.0);
  viewport.devicePixelRatio = 1.0;
  viewport.resetTo100Percent();
  const EditorRasterViewport visibleRaster = viewport.rasterViewport();
  ASSERT_TRUE(visibleRaster.viewportBounded);
  ASSERT_GT(viewport.selectedPrewarmRasterViewport().outputSizePx.x, visibleRaster.outputSizePx.x);

  RenderCoordinator coordinator;
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  const std::uint64_t generation = app.document().documentGeneration();
  const std::uint64_t version = app.document().currentFrameVersion();
  const Entity selectedEntity = target->unsafeEntityHandle().entity();
  for (std::size_t attempt = 0; attempt <= NothingToPresentRetry::kRetryDelays.size(); ++attempt) {
    RenderCoordinatorTestAccess::noteSelectedPrewarmFailure(coordinator, generation, version,
                                                            selectedEntity, viewport);
  }
  ASSERT_TRUE(RenderCoordinatorTestAccess::selectedPrewarmRecoveryPending(coordinator));

  SelectTool selectTool;
  ASSERT_TRUE(coordinator.maybeRequestRender(app, selectTool, viewport, nullptr));
  const auto postedRaster = RenderCoordinatorTestAccess::lastPostedRasterViewport(coordinator);
  ASSERT_TRUE(postedRaster.has_value());
  EXPECT_EQ(postedRaster->outputSizePx, visibleRaster.outputSizePx)
      << "the bounded recovery must post even when generic retries for enlarged prewarm ended";
  EXPECT_EQ(postedRaster->documentRect, visibleRaster.documentRect);
  EXPECT_TRUE(RenderCoordinatorTestAccess::selectedPrewarmFallbackApplies(
      coordinator, generation, selectedEntity, visibleRaster));

  EXPECT_FALSE(RenderCoordinatorTestAccess::selectedPrewarmFallbackApplies(
      coordinator, generation, peer->unsafeEntityHandle().entity(), visibleRaster));
  EXPECT_FALSE(RenderCoordinatorTestAccess::selectedPrewarmRecoveryPending(coordinator));

  RenderCoordinatorTestAccess::noteSelectedPrewarmFailure(coordinator, generation, version,
                                                          selectedEntity, viewport);
  ViewportState pannedViewport = viewport;
  pannedViewport.panDocPoint.x += 10.0;
  EXPECT_FALSE(RenderCoordinatorTestAccess::selectedPrewarmFallbackApplies(
      coordinator, generation, selectedEntity, pannedViewport.rasterViewport()));

  RenderCoordinatorTestAccess::noteSelectedPrewarmFailure(coordinator, generation, version,
                                                          selectedEntity, viewport);
  EXPECT_FALSE(RenderCoordinatorTestAccess::selectedPrewarmFallbackApplies(
      coordinator, generation + 1u, selectedEntity, visibleRaster));
}

// A renderer setting changes what the worker draws without changing the document or the raster:
// the composited mode, the geometry debug pass, arming the eyedropper. Each asks for a presentation
// refresh, and the request made for it is not the request whose retries ran out.
TEST(RenderCoordinatorTest, PresentationRefreshIsPostedAfterRetriesRunOut) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);
  ASSERT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1);
  for (const std::chrono::milliseconds delay : NothingToPresentRetry::kRetryDelays) {
    RenderCoordinatorTestAccess::advanceFakeRetryClock(delay);
    ASSERT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1);
  }
  ASSERT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 0)
      << "the failed request is held once its retries run out";

  coordinator.requestPresentationRefresh();
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1)
      << "a renderer-setting change must reach the worker even after the retries ran out";
}

// When a retry falls due, the idle loop has already woken for it, and the next frame that asks for
// a render posts it. A frame that cannot ask for one, such as while the sample picker is open, must
// not be told to wake again at once, or the idle loop spins until the user acts.
TEST(RenderCoordinatorTest, DueRetryDoesNotKeepTheIdleLoopAwake) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);
  ASSERT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1);
  ASSERT_THAT(coordinator.nextNothingToPresentRetryWakeSeconds(),
              ::testing::Optional(::testing::FloatNear(0.1f, 1e-4f)));

  RenderCoordinatorTestAccess::advanceFakeRetryClock(NothingToPresentRetry::kRetryDelays.front());
  coordinator.pollRenderResult(app, viewport, textures);
  EXPECT_EQ(coordinator.nextNothingToPresentRetryWakeSeconds(), std::nullopt)
      << "a due retry must not ask the idle loop to wake again";
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 1), 1)
      << "the next frame that asks for a render posts the due retry";
}

// Pacing holds back only the request that failed. A new document version, or a replaced document,
// is a different request and is posted at once.
TEST(RenderCoordinatorTest, NothingToPresentRetryStartsOverForANewVersionOrDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);
  ASSERT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1);

  app.applyMutation(EditorCommand::SetAttributeCommand(QuerySelector(app, "#r1"), "fill", "green"));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1)
      << "an edit makes a new request, which gets its own attempt";

  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  coordinator.resetForLoadedDocument(app.document().documentGeneration());
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1)
      << "a replaced document starts over";
  EXPECT_EQ(coordinator.displayedDocVersionForDiagnostics(), 0u);
}

// The eyedropper keeps asking for a document pixel capture until one lands. A capture render that
// comes back with nothing to present must not re-arm that request on every frame either.
TEST(RenderCoordinatorTest, PixelCaptureWithNothingToPresentIsNotRepostedEveryFrame) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  SelectTool selectTool;
  const ViewportState viewport = MakeViewport(app);
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);
  coordinator.setDocumentPixelCaptureEnabled(true);

  const int posted = CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10);
  EXPECT_THAT(posted, ::testing::AllOf(::testing::Ge(1), ::testing::Le(2)))
      << "ten idle frames of a capture render that keeps producing nothing to present";
  EXPECT_EQ(coordinator.documentPixelCaptureFor(app, viewport), nullptr);
  EXPECT_FALSE(coordinator.documentPixelCaptureUnavailable())
      << "a retry is still scheduled, so the capture is not given up yet";

  for (const std::chrono::milliseconds delay : NothingToPresentRetry::kRetryDelays) {
    RenderCoordinatorTestAccess::advanceFakeRetryClock(delay);
    EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 1);
  }
  EXPECT_TRUE(coordinator.documentPixelCaptureUnavailable())
      << "after the last retry fails, the picker reports the capture unavailable";
  RenderCoordinatorTestAccess::advanceFakeRetryClock(std::chrono::minutes(1));
  EXPECT_EQ(CountPostedRenders(coordinator, app, selectTool, viewport, textures, 10), 0)
      << "an unavailable capture is not requested again for the same document and viewport";
}

}  // namespace
TEST(RenderCoordinatorTest, RejectedCaptureHasBoundedRetriesWithoutConfigurationInvalidation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  GlTextureCache textures;
  RenderCoordinatorTestAccess::useFakeRetryClock(coordinator);
  RenderResult result;
  result.documentGeneration = app.document().documentGeneration();
  result.version = app.document().currentFrameVersion();
  result.rasterViewport = MakeViewport(app).rasterViewport();
  result.compositedPreview = RenderResult::CompositedPreview{};
  for (const auto delay : NothingToPresentRetry::kRetryDelays) {
    EXPECT_FALSE(
        RenderCoordinatorTestAccess::rejectPreparedResult(coordinator, result, app, textures));
    EXPECT_THAT(
        coordinator.nextNothingToPresentRetryWakeSeconds(),
        testing::Optional(testing::FloatNear(std::chrono::duration<float>(delay).count(), 1e-4f)));
    EXPECT_FALSE(coordinator.presentationRefreshPending())
        << "a rejected capture is repair work, not a renderer-setting invalidation";
    RenderCoordinatorTestAccess::advanceFakeRetryClock(delay);
  }
  EXPECT_FALSE(
      RenderCoordinatorTestAccess::rejectPreparedResult(coordinator, result, app, textures));
  EXPECT_EQ(coordinator.nextNothingToPresentRetryWakeSeconds(), std::nullopt);
}

TEST(RenderCoordinatorTest, CurrentOverviewCanReplaceUnusableBoundedCoverage) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto capture = AcceptCapturedScene(coordinator, app);
  const auto resources = coordinator.compositedPresentation().resources();
  coordinator.compositedPresentation().notePreparedResources(
      FramePresentationTestAccess::resources(
          capture, resources->tiles(),
          {.activeTilesViewportBounded = true,
           .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 1, 1)}),
      capture->selection().front(), std::nullopt);
  EXPECT_EQ(BuildFrame(coordinator, app, tool, MakeViewport(app), textures), nullptr);
  EXPECT_GT(coordinator.lastFrameCostBreakdown().overlay.captureMs, 0.0);
  RenderResult overview;
  overview.capturedPresentation = capture;
  overview.version = app.document().currentFrameVersion();
  overview.documentGeneration = app.document().documentGeneration();
  overview.overviewInfillOnly = true;
  EXPECT_TRUE(RenderCoordinatorTestAccess::canReplaceWithOverview(coordinator, overview, app))
      << "a current coherent overview must unblock a frame whose bounded raster lacks coverage";
  for (const auto failure : {FramePresentationFailure::MissingSelectionGeometry,
                             FramePresentationFailure::InsufficientCoverage,
                             FramePresentationFailure::IncompatiblePose}) {
    RenderCoordinatorTestAccess::changePendingRepairFailure(coordinator, failure);
    EXPECT_TRUE(RenderCoordinatorTestAccess::canReplaceWithOverview(coordinator, overview, app))
        << "A complete current family must also repair missing selection or projected coverage.";
  }
}

TEST(RenderCoordinatorTest, RepairBudgetSurvivesOverviewAndBoundedRasterStages) {
  NothingToPresentRetry retry;
  const auto now = NothingToPresentRetry::Clock::now();
  RenderAttemptIdentity overview;
  overview.overviewInfillOnly = true;
  overview.repair = PresentationRepairIdentity{
      .scene = {.documentGeneration = 1, .documentRevision = 2, .version = 3},
      .failure = FramePresentationFailure::MissingOverview};
  retry.noteFailure(overview, now);
  auto bounded = overview;
  bounded.overviewInfillOnly = false;
  bounded.rasterViewport.outputSizePx = Vector2i(1024, 1024);
  bounded.repair->failure = FramePresentationFailure::InsufficientCoverage;
  EXPECT_FALSE(retry.mayPost(bounded, now + std::chrono::milliseconds(50)));
  EXPECT_TRUE(retry.mayPost(bounded, now + std::chrono::milliseconds(100)));
  bounded.repair->coverage.outputSizePx = Vector2i(2048, 2048);
  EXPECT_TRUE(retry.mayPost(bounded, now + std::chrono::milliseconds(50)))
      << "a changed coverage requirement is new repair intent";
}

TEST(RenderCoordinatorTest, CompatibleOverviewPreservesNewerActiveDragIntent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto capture = AcceptCapturedScene(coordinator, app);
  const auto resources = coordinator.compositedPresentation().resources();
  tool.onMouseMove(app, Vector2d(35.0, 15.0), true);
  ASSERT_TRUE(app.flushFrame());
  coordinator.compositedPresentation().notePreparedResources(
      FramePresentationTestAccess::resources(
          capture, resources->tiles(),
          {.activeTilesViewportBounded = true,
           .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 1, 1)}),
      capture->selection().front(), std::nullopt);
  EXPECT_EQ(BuildFrame(coordinator, app, tool, MakeViewport(app), textures), nullptr);
  RenderResult overview;
  overview.capturedPresentation = capture;
  overview.version = capture->identity().version;
  overview.documentGeneration = app.document().documentGeneration();
  overview.overviewInfillOnly = true;
  EXPECT_TRUE(RenderCoordinatorTestAccess::canReplaceWithOverview(coordinator, overview, app));
  coordinator.compositedPresentation().notePreparedResources(
      resources, capture->selection().front(), std::nullopt);
  const auto frame = BuildFrame(coordinator, app, tool, MakeViewport(app), textures);
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->followsPointer());
  EXPECT_EQ(frame->chrome().paths.front().pathDoc.bounds(), Box2d::FromXYWH(30, 10, 20, 20));
}

TEST(RenderCoordinatorTest, SameVersionOverviewRepairRequestsMissingSceneCoverage) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  RenderCoordinator coordinator;
  SelectTool tool;
  GlTextureCache textures;
  const auto overview = AcceptCapturedScene(coordinator, app);
  const auto oldResources = coordinator.compositedPresentation().resources();
  app.document().document().setCanvasSize(200, 200);
  const auto active = AcceptCapturedScene(coordinator, app);
  ASSERT_EQ(active->identity().version, overview->identity().version);
  ASSERT_NE(active->identity().documentRevision, overview->identity().documentRevision);
  coordinator.compositedPresentation().notePreparedResources(
      FramePresentationTestAccess::resources(
          active, oldResources->tiles(),
          {.activeTilesViewportBounded = true,
           .overviewInfillAvailable = true,
           .activeRasterDocumentRect = Box2d::FromXYWH(0, 0, 1, 1)},
          overview, oldResources->tiles()),
      active->selection().front(), std::nullopt);
  EXPECT_EQ(BuildFrame(coordinator, app, tool, MakeViewport(app), textures), nullptr);
  EXPECT_TRUE(RenderCoordinatorTestAccess::requiresFreshOverview(
      coordinator, true, app.document().currentFrameVersion()));
}

TEST(RenderCoordinatorTest, UnpairedBoundedResultCannotReplaceCompleteCommittedScene) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  RenderCoordinator coordinator;
  const auto old = AcceptCapturedScene(coordinator, app);
  const auto oldTiles = coordinator.compositedPresentation().resources()->tiles();
  const auto second = QuerySelector(app, "#r2");
  app.document().applyMutation(
      EditorCommand::SetTransformCommand(second, Transform2d::Translate(20, 0)));
  ASSERT_TRUE(app.flushFrame());
  const auto current = AcceptCapturedScene(coordinator, app);
  ASSERT_TRUE(old->identity().sameContent(current->identity()));
  GlTextureCache textures;
  FramePresentationTestAccess::installResources(
      textures, FramePresentationTestAccess::resources(current, oldTiles, {}, old, oldTiles));
  RenderResult result;
  result.version = app.document().currentFrameVersion();
  result.capturedPresentation = current;
  result.rasterViewport.viewportBounded = true;
  result.compositedPreview = RenderResult::CompositedPreview{};
  result.compositedPreview->representedDragPreview =
      RenderRequest::DragPreview{.entity = QuerySelector(app, "#r1").unsafeEntityHandle().entity(),
                                 .interactionKind = svg::compositor::InteractionHint::ActiveDrag};
  EXPECT_FALSE(RenderCoordinatorTestAccess::canPresentWithOverview(
      coordinator, result, result.rasterViewport, app, textures));
}

TEST(RenderCoordinatorTest, PendingOverviewRequiresExactSceneBeyondFrameVersion) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  app.setSelection(QuerySelector(app, "#r1"));
  RenderCoordinator coordinator;
  const auto old = AcceptCapturedScene(coordinator, app);
  app.document().document().setCanvasSize(200, 200);
  const auto current = AcceptCapturedScene(coordinator, app);
  ASSERT_EQ(old->identity().version, current->identity().version);
  ASSERT_NE(old->identity().documentRevision, current->identity().documentRevision);
  RenderResult result;
  result.documentGeneration = app.document().documentGeneration();
  result.version = app.document().currentFrameVersion();
  result.capturedPresentation = current;
  EXPECT_FALSE(RenderCoordinatorTestAccess::matchingPendingOverview(coordinator, old, result, app));
  EXPECT_TRUE(
      RenderCoordinatorTestAccess::matchingPendingOverview(coordinator, current, result, app));
}

}  // namespace donner::editor
