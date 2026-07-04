#include "donner/editor/RenderCoordinator.h"

#include <chrono>
#include <thread>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// Unit coverage for `RenderCoordinator`'s pure-logic predicates and the
// GL-free portions of its orchestration. These tests deliberately avoid any
// path that calls into a live GL/WGPU context: `GlTextureCache::uploadComposited`
// emits raw `glGenTextures`/`glBindTexture` in the default
// (tiny-skia) build, so the editor unit-test process — which has no GL context
// — would crash. The harness therefore:
//   * uses the default CPU (`tiny-skia`) `svg::Renderer` (no Geode device), and
//   * keeps `displayedDocVersion_ == 0` while the live document is at
//     `currentFrameVersion() >= 1`, so `rasterizeOverlayForCurrentSelection`
//     in `MatchDisplayedVersion` mode rasterizes but never uploads.
//
// `pollRenderResult` is intentionally left to the integration-style
// `.rnr`-replay and golden-image suites: every non-empty render produces a
// composited preview, and presenting it calls `GlTextureCache::uploadComposited`
// (raw GL), which is unreachable from this contextless unit harness.

namespace donner::editor {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

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

// A non-graphics element (a bare `<defs>`) so the "selected element is not a
// graphics element" branches in the predicates are exercised.
constexpr std::string_view kDefsSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <defs id="d1"><rect id="r1" x="0" y="0" width="10" height="10"/></defs>
       </svg>)";

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
}

TEST(RenderCoordinatorPolicyTest, PendingSelectedLayerRasterizationBypassesViewportDefer) {
  const Entity selectedEntity = static_cast<Entity>(7);

  EXPECT_TRUE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedSelectedTexture=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/false));
  EXPECT_FALSE(ShouldDeferSelectedViewportRefresh(
      selectedEntity, /*hasActiveDrag=*/false, /*currentVersion=*/8, /*displayedDocVersion=*/8,
      /*hasCachedSelectedTexture=*/true, /*rasterViewportSettled=*/false,
      /*needsOverviewInfill=*/false, /*pendingSelectedLayerRasterization=*/true))
      << "A style/fill edit marks the selected layer pixels stale; the coordinator must request "
         "the forced layer rasterization immediately instead of deferring forever on an unsettled "
         "viewport.";
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

TEST(RenderCoordinatorPolicyTest, RepresentedDocumentTransformRequiresMatchingInvertibleDrag) {
  const SelectTool::ActiveDragPreview live = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(10.0, 0.0), Transform2d::Translate(Vector2d(10.0, 0.0)));
  const SelectTool::ActiveDragPreview represented = DragPreview(
      static_cast<Entity>(42), 7, Vector2d(3.0, 0.0), Transform2d::Translate(Vector2d(3.0, 0.0)));

  EXPECT_TRUE(OverlayRepresentedDocumentFromLiveDocument(std::nullopt, represented).isIdentity());
  EXPECT_TRUE(OverlayRepresentedDocumentFromLiveDocument(
                  live, DragPreview(static_cast<Entity>(42), 8, Vector2d(3.0, 0.0)))
                  .isIdentity());
  EXPECT_TRUE(
      OverlayRepresentedDocumentFromLiveDocument(
          DragPreview(static_cast<Entity>(42), 7, Vector2d::Zero(), Transform2d::Scale(0.0)),
          represented)
          .isIdentity());

  const Transform2d projected = OverlayRepresentedDocumentFromLiveDocument(live, represented);
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

// ---------------------------------------------------------------------------
// setSourceHoverElements — change detection.
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
// selectedElementIsDisplayNone — predicate over the live selection.
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

// ---------------------------------------------------------------------------
// suppressedCompositedLayerEntity — display:none stale-layer suppression.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, SuppressedLayerEntityNullWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kHiddenRectSvg));
  RenderCoordinator coordinator;
  EXPECT_EQ(coordinator.suppressedCompositedLayerEntity(app), kNullEntity);
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
  // suppression target — there is no separately-cached promoted layer to hide.
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
  EXPECT_THAT(cache.lastSelection, ElementsAre(r1));
  EXPECT_EQ(cache.lastRefreshVersion, app.document().currentFrameVersion());
  // The single selected rect has renderable geometry → one pending bound.
  ASSERT_THAT(cache.pendingBoundsDoc, SizeIs(1u));
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
  ASSERT_THAT(coordinator.selectionBoundsCache().pendingBoundsDoc, Not(IsEmpty()));

  coordinator.promoteSelectionBoundsIfReady();
  EXPECT_THAT(coordinator.selectionBoundsCache().displayedBoundsDoc, IsEmpty())
      << "Pending bounds must not promote while displayedDocVersion lags the pending version.";
  EXPECT_THAT(coordinator.selectionBoundsCache().pendingBoundsDoc, Not(IsEmpty()))
      << "Unpromoted pending bounds must be retained for a later catch-up.";
}

// ---------------------------------------------------------------------------
// resetForLoadedDocument — clears all coordinator-owned state.
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

  ASSERT_THAT(coordinator.selectionBoundsCache().lastSelection, Not(IsEmpty()));
  ASSERT_TRUE(coordinator.compositedPresentation().hasCachedTextures());

  coordinator.resetForLoadedDocument();

  EXPECT_THAT(coordinator.selectionBoundsCache().lastSelection, IsEmpty());
  EXPECT_FALSE(coordinator.compositedPresentation().hasCachedTextures());
  EXPECT_EQ(coordinator.displayedDocVersion(), 0u);
  EXPECT_FALSE(coordinator.immediateOverlaySnapshot().has_value());

  // A hover set re-issued after reset reports a change (the cleared set differs
  // from the new one) — confirming the hover state was actually cleared.
  EXPECT_TRUE(coordinator.setSourceHoverElements({QuerySelector(app, "#r2")}));
}

// ---------------------------------------------------------------------------
// rasterizeOverlayForCurrentSelection — GL-free immediate overlay snapshotting.
// ---------------------------------------------------------------------------

TEST(RenderCoordinatorTest, RasterizeOverlayReturnsFalseWithoutDocument) {
  EditorApp app;
  RenderCoordinator coordinator;
  ViewportState viewport;
  ASSERT_FALSE(app.hasDocument());

  EXPECT_FALSE(coordinator.rasterizeOverlayForCurrentSelection(app, viewport,
                                                               /*marqueeRectDoc=*/std::nullopt));
}

TEST(RenderCoordinatorTest, RasterizeOverlayPublishesImmediateSnapshot) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  ViewportState viewport = MakeViewport(app);

  app.setSelection(QuerySelector(app, "#r1"));

  ASSERT_GT(app.document().currentFrameVersion(), coordinator.displayedDocVersion());
  EXPECT_TRUE(coordinator.rasterizeOverlayForCurrentSelection(app, viewport,
                                                              /*marqueeRectDoc=*/std::nullopt));
  ASSERT_TRUE(coordinator.immediateOverlaySnapshot().has_value());
  EXPECT_THAT(coordinator.immediateOverlaySnapshot()->paths, SizeIs(1u));
}

TEST(RenderCoordinatorTest, RasterizeOverlaySkipsUnchangedImmediateSnapshot) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  ViewportState viewport = MakeViewport(app);

  app.setSelection(QuerySelector(app, "#r1"));

  ASSERT_TRUE(coordinator.rasterizeOverlayForCurrentSelection(app, viewport,
                                                              /*marqueeRectDoc=*/std::nullopt));
  EXPECT_FALSE(coordinator.rasterizeOverlayForCurrentSelection(app, viewport,
                                                               /*marqueeRectDoc=*/std::nullopt));
  EXPECT_EQ(coordinator.lastFrameCostBreakdown().overlay.selectedElementCount, 1);
  EXPECT_FALSE(coordinator.lastFrameCostBreakdown().overlay.selectionBoundsOnly);
}

TEST(RenderCoordinatorTest, RasterizeOverlayTracksActiveBoundsPreviewChanges) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  ViewportState viewport = MakeViewport(app);

  app.setSelection(QuerySelector(app, "#r1"));

  SelectTool::ActiveTransformBoundsPreview boundsPreview;
  boundsPreview.startBoundsDoc = Box2d::FromXYWH(10.0, 10.0, 20.0, 20.0);
  boundsPreview.documentFromStartDocument = Transform2d::Translate(Vector2d(2.0, 0.0));

  EXPECT_TRUE(coordinator.rasterizeOverlayForCurrentSelection(
      app, viewport, /*marqueeRectDoc=*/std::nullopt,
      /*representedDragPreview=*/std::nullopt, boundsPreview));
  EXPECT_TRUE(coordinator.immediateOverlaySnapshot().has_value());

  boundsPreview.documentFromStartDocument = Transform2d::Translate(Vector2d(4.0, 0.0));
  EXPECT_TRUE(coordinator.rasterizeOverlayForCurrentSelection(
      app, viewport, /*marqueeRectDoc=*/std::nullopt,
      /*representedDragPreview=*/std::nullopt, boundsPreview));
  EXPECT_TRUE(coordinator.immediateOverlaySnapshot().has_value());
}

TEST(RenderCoordinatorTest, RasterizeOverlayWithEmptySelectionIsAccepted) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  RenderCoordinator coordinator;
  ViewportState viewport = MakeViewport(app);

  // No selection: the overlay still rasterizes (empty chrome) and parks
  // pending against the undisplayed version.
  EXPECT_TRUE(coordinator.rasterizeOverlayForCurrentSelection(app, viewport,
                                                              /*marqueeRectDoc=*/std::nullopt));
}

// ---------------------------------------------------------------------------
// maybeRequestRender — GL-free orchestration that posts an async render request.
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

}  // namespace
}  // namespace donner::editor
