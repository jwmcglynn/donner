#include "donner/editor/DocumentPresenter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

/// A 200x100 pane at the screen origin, with the document drawn 1:1 at (10, 20).
ViewportState TestViewport() {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(0.0, 0.0);
  viewport.paneSize = Vector2d(200.0, 100.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 40.0, 30.0);
  viewport.zoom = 1.0;
  viewport.panDocPoint = Vector2d(0.0, 0.0);
  viewport.panScreenPoint = Vector2d(10.0, 20.0);
  return viewport;
}

Box2d TestPaneRect() {
  return Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
}

DirectSurfacePresentationState ActiveSurface(std::uint64_t frameCount, int surfaceSlot = 0,
                                             bool selectionChromeBaked = false) {
  DirectSurfacePresentationState surface;
  surface.active = true;
  surface.rasterViewport.documentRect = Box2d::FromXYWH(0.0, 0.0, 40.0, 30.0);
  // Epochs published by the worker carry the viewport they were rasterized
  // against; the default epoch matches the live test viewport exactly.
  surface.viewport = TestViewport();
  surface.frameCount = frameCount;
  surface.surfaceSlot = surfaceSlot;
  surface.selectionChromeBaked = selectionChromeBaked;
  return surface;
}

/// A viewport-bounded high-zoom epoch: the raster covers the 200x100 pane plus a
/// 32-screen-pixel margin on each side, at 4 screen pixels per document unit.
DirectSurfacePresentationState BoundedActiveSurface(std::uint64_t frameCount = 3) {
  ViewportState epoch = TestViewport();
  epoch.zoom = 4.0;
  epoch.panDocPoint = Vector2d(0.0, 0.0);
  epoch.panScreenPoint = Vector2d(0.0, 0.0);

  DirectSurfacePresentationState surface;
  surface.active = true;
  surface.viewport = epoch;
  surface.rasterViewport.viewportBounded = true;
  surface.rasterViewport.documentRect =
      epoch.screenToDocument(Box2d(Vector2d(-32.0, -32.0), Vector2d(232.0, 132.0)));
  surface.rasterViewport.outputSizePx = Vector2i(264, 164);
  surface.frameCount = frameCount;
  return surface;
}

/// Screen rect the layout's pixels are actually placed over.
Box2d LayoutRect(const WorkerSurfaceLayout& layout) {
  return Box2d::FromXYWH(layout.left, layout.top, layout.width, layout.height);
}

DocumentPresentationFrame TestFrame(DirectSurfacePresentationState surface,
                                    bool presentationSuppressed = false) {
  return DocumentPresentationFrame{
      .viewport = TestViewport(),
      .paneRect = TestPaneRect(),
      .presentationSuppressed = presentationSuppressed,
      .workerSurface = std::move(surface),
  };
}

/// Records everything the presenter under test hands to its sinks.
struct RecordingSinks {
  std::vector<WorkerSurfaceLayout> layouts;
  std::vector<std::optional<FramebufferUnderlayPlan>> plans;

  WorkerSurfaceLayoutSink layoutSink() {
    return [this](const WorkerSurfaceLayout& layout) { layouts.push_back(layout); };
  }

  FramebufferUnderlayPlanSink planSink() {
    return
        [this](std::optional<FramebufferUnderlayPlan> plan) { plans.push_back(std::move(plan)); };
  }

  /// The plans actually installed, ignoring the clear that precedes each install.
  [[nodiscard]] std::vector<const FramebufferUnderlayPlan*> installedPlans() const {
    std::vector<const FramebufferUnderlayPlan*> installed;
    for (const std::optional<FramebufferUnderlayPlan>& plan : plans) {
      if (plan.has_value()) {
        installed.push_back(&*plan);
      }
    }
    return installed;
  }
};

FramebufferUnderlayPlan TestPlan() {
  FramebufferUnderlayPlan plan;
  plan.viewport = TestViewport();
  plan.documentClipRect = Box2d::FromXYWH(10.0, 20.0, 40.0, 30.0);
  plan.suppressDragTargetTiles = true;
  return plan;
}

// ---------------------------------------------------------------------------
// Presenter selection
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, FramebufferTargetSelectsTheUnderlayPresenter) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  EXPECT_FALSE(presenter->presentsToExternalSurface());

  // Even with an accepted worker surface, this target never presents through one.
  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(ActiveSurface(7)));
  EXPECT_FALSE(result.documentPresentedDirectly);
  EXPECT_FALSE(result.externalSurfacePresented);
  EXPECT_FALSE(result.selectionChromeBaked);
  EXPECT_TRUE(sinks.layouts.empty());
}

TEST(DocumentPresenterTest, WorkerSurfaceTargetSelectsTheWorkerPresenter) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  EXPECT_TRUE(presenter->presentsToExternalSurface());

  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(ActiveSurface(7)));
  EXPECT_TRUE(result.documentPresentedDirectly);
  EXPECT_TRUE(result.externalSurfacePresented);
  ASSERT_EQ(sinks.layouts.size(), 1u);
  EXPECT_TRUE(sinks.layouts.front().visible);
}

TEST(DocumentPresenterTest, DefaultTargetMatchesTheBuild) {
#if defined(DONNER_EDITOR_WGPU) && defined(__EMSCRIPTEN__)
  EXPECT_EQ(DefaultDocumentPresentationTarget(), DocumentPresentationTarget::WorkerSurface);
#else
  EXPECT_EQ(DefaultDocumentPresentationTarget(), DocumentPresentationTarget::FramebufferUnderlay);
#endif
}

// ---------------------------------------------------------------------------
// Worker surface placement
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, WorkerSurfaceLayoutMapsTheDocumentRectThroughTheViewport) {
  const std::optional<WorkerSurfaceLayout> layout =
      ComputeWorkerSurfaceLayout(TestFrame(ActiveSurface(3)));

  ASSERT_TRUE(layout.has_value());
  EXPECT_TRUE(layout->visible);
  EXPECT_DOUBLE_EQ(layout->left, 10.0);
  EXPECT_DOUBLE_EQ(layout->top, 20.0);
  EXPECT_DOUBLE_EQ(layout->width, 40.0);
  EXPECT_DOUBLE_EQ(layout->height, 30.0);
  // Fully inside the pane, so no clip insets.
  EXPECT_DOUBLE_EQ(layout->clipLeft, 0.0);
  EXPECT_DOUBLE_EQ(layout->clipTop, 0.0);
  EXPECT_DOUBLE_EQ(layout->clipRight, 0.0);
  EXPECT_DOUBLE_EQ(layout->clipBottom, 0.0);
}

TEST(DocumentPresenterTest, WorkerSurfaceLayoutCarriesPaneClipInsets) {
  DirectSurfacePresentationState surface = ActiveSurface(3);
  // Slide the document up and left so it overhangs the pane's top-left corner.
  // The epoch was rasterized there, so its own viewport carries the pan.
  surface.viewport.panScreenPoint = Vector2d(-8.0, -6.0);
  DocumentPresentationFrame frame = TestFrame(surface);
  frame.viewport.panScreenPoint = Vector2d(-8.0, -6.0);

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, -8.0);
  EXPECT_DOUBLE_EQ(layout->top, -6.0);
  EXPECT_DOUBLE_EQ(layout->clipLeft, 8.0);
  EXPECT_DOUBLE_EQ(layout->clipTop, 6.0);
  EXPECT_DOUBLE_EQ(layout->clipRight, 0.0);
  EXPECT_DOUBLE_EQ(layout->clipBottom, 0.0);
}

TEST(DocumentPresenterTest, WorkerSurfaceHiddenWhenScrolledOutOfThePane) {
  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.viewport.panScreenPoint = Vector2d(500.0, 500.0);
  DocumentPresentationFrame frame = TestFrame(surface);
  frame.viewport.panScreenPoint = Vector2d(500.0, 500.0);

  EXPECT_FALSE(ComputeWorkerSurfaceLayout(frame).has_value());
}

TEST(DocumentPresenterTest, WorkerSurfaceHiddenWhenTheWorkerHasNoAcceptedSurface) {
  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.active = false;

  EXPECT_FALSE(ComputeWorkerSurfaceLayout(TestFrame(surface)).has_value());
}

TEST(DocumentPresenterTest, WorkerSurfaceHiddenWhilePresentationIsSuppressed) {
  EXPECT_FALSE(
      ComputeWorkerSurfaceLayout(TestFrame(ActiveSurface(3), /*presentationSuppressed=*/true))
          .has_value());
}

// ---------------------------------------------------------------------------
// Epoch-consistent placement
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, EpochViewportMatchingTheLiveViewportPlacesTheSurfaceUnchanged) {
  // Same-epoch: the live viewport and the epoch's viewport agree, so epoch
  // placement is a no-op against the historical live-viewport placement.
  const std::optional<WorkerSurfaceLayout> layout =
      ComputeWorkerSurfaceLayout(TestFrame(ActiveSurface(3)));

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, 10.0);
  EXPECT_DOUBLE_EQ(layout->top, 20.0);
  EXPECT_DOUBLE_EQ(layout->width, 40.0);
  EXPECT_DOUBLE_EQ(layout->height, 30.0);
}

TEST(DocumentPresenterTest, LiveZoomAheadOfTheEpochKeepsTheEpochPlacement) {
  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
  // The user has zoomed to 300% since the accepted epoch was rasterized. The
  // epoch's 40x30 document rect still only holds 40x30 screen pixels' worth of
  // content, so stretching it to 120x90 would magnify pixels past their
  // coverage instead of showing the region they cover.
  frame.viewport.zoom = 3.0;

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, 10.0);
  EXPECT_DOUBLE_EQ(layout->top, 20.0);
  EXPECT_DOUBLE_EQ(layout->width, 40.0);
  EXPECT_DOUBLE_EQ(layout->height, 30.0);
}

TEST(DocumentPresenterTest, LivePanAheadOfTheEpochKeepsTheEpochPlacement) {
  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
  frame.viewport.panScreenPoint = Vector2d(60.0, 45.0);

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, 10.0);
  EXPECT_DOUBLE_EQ(layout->top, 20.0);
  EXPECT_DOUBLE_EQ(layout->width, 40.0);
  EXPECT_DOUBLE_EQ(layout->height, 30.0);
}

TEST(DocumentPresenterTest, EpochNewerThanTheLiveViewportStillPlacesByItsOwnTransform) {
  // A race the other way: the worker rasterized against a viewport the UI thread
  // has not observed yet (its own snapshot is a frame behind). The pixels still
  // belong where the epoch says, not where the older live viewport says.
  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.viewport.zoom = 2.0;
  surface.viewport.panScreenPoint = Vector2d(4.0, 6.0);

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(TestFrame(surface));

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, 4.0);
  EXPECT_DOUBLE_EQ(layout->top, 6.0);
  EXPECT_DOUBLE_EQ(layout->width, 80.0);
  EXPECT_DOUBLE_EQ(layout->height, 60.0);
}

TEST(DocumentPresenterTest, EpochPlacementNeverStretchesPastTheRasterItCovers) {
  const DirectSurfacePresentationState surface = BoundedActiveSurface();
  DocumentPresentationFrame frame = TestFrame(surface);
  frame.viewport.zoom = 1.0;  // Zoomed far out since the epoch was rasterized.

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  // One raster device pixel per screen pixel at the epoch's device pixel ratio:
  // the surface is placed at exactly the size its pixels cover.
  EXPECT_DOUBLE_EQ(layout->width * surface.viewport.devicePixelRatio,
                   static_cast<double>(surface.rasterViewport.outputSizePx.x));
  EXPECT_DOUBLE_EQ(layout->height * surface.viewport.devicePixelRatio,
                   static_cast<double>(surface.rasterViewport.outputSizePx.y));
}

TEST(DocumentPresenterTest, ZoomingOutAheadOfABoundedEpochStillCoversThePane) {
  // The B2 regression: a viewport-bounded epoch covers the pane plus margin at
  // its own zoom. Mapping its document rect through a zoomed-out live viewport
  // shrinks the surface inside the pane and uncovers the editor background.
  const DirectSurfacePresentationState surface = BoundedActiveSurface();
  DocumentPresentationFrame frame = TestFrame(surface);
  frame.viewport.zoom = 1.0;

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  const Box2d placed = LayoutRect(*layout);
  EXPECT_LE(placed.topLeft.x, frame.paneRect.topLeft.x);
  EXPECT_LE(placed.topLeft.y, frame.paneRect.topLeft.y);
  EXPECT_GE(placed.bottomRight.x, frame.paneRect.bottomRight.x);
  EXPECT_GE(placed.bottomRight.y, frame.paneRect.bottomRight.y);
  // The pane clip trims the margin the epoch rasterized outside the pane.
  EXPECT_DOUBLE_EQ(layout->clipLeft, 32.0);
  EXPECT_DOUBLE_EQ(layout->clipTop, 32.0);
  EXPECT_DOUBLE_EQ(layout->clipRight, 32.0);
  EXPECT_DOUBLE_EQ(layout->clipBottom, 32.0);
}

TEST(DocumentPresenterTest, ADegenerateEpochViewportFallsBackToTheLiveViewport) {
  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.viewport = ViewportState{};  // No pane, as published before this field existed.
  DocumentPresentationFrame frame = TestFrame(surface);
  frame.viewport.zoom = 2.0;

  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, 10.0);
  EXPECT_DOUBLE_EQ(layout->top, 20.0);
  EXPECT_DOUBLE_EQ(layout->width, 80.0);
  EXPECT_DOUBLE_EQ(layout->height, 60.0);
}

TEST(DocumentPresenterTest, TheLivePaneStillOwnsTheClip) {
  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.viewport.panScreenPoint = Vector2d(-8.0, -6.0);

  DocumentPresentationFrame frame = TestFrame(surface);
  const std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);

  ASSERT_TRUE(layout.has_value());
  EXPECT_DOUBLE_EQ(layout->left, -8.0);
  EXPECT_DOUBLE_EQ(layout->top, -6.0);
  EXPECT_DOUBLE_EQ(layout->clipLeft, 8.0);
  EXPECT_DOUBLE_EQ(layout->clipTop, 6.0);
}

// ---------------------------------------------------------------------------
// Presented viewport
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, TheUnderlayPresentsInTheLiveViewport) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
  frame.viewport.zoom = 2.5;

  const DocumentPresentationResult result = presenter->resolveExternalSurface(frame);

  EXPECT_DOUBLE_EQ(result.presentedViewport.zoom, 2.5);
}

TEST(DocumentPresenterTest, TheWorkerSurfacePresentsInTheAcceptedEpochsViewport) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState surface = ActiveSurface(3);
  surface.viewport.zoom = 1.0;
  DocumentPresentationFrame frame = TestFrame(surface);
  // The UI thread has zoomed since the worker rasterized this epoch. Chrome
  // drawn at 3.0 would float away from pixels placed at 1.0.
  frame.viewport.zoom = 3.0;

  const DocumentPresentationResult result = presenter->resolveExternalSurface(frame);

  ASSERT_TRUE(result.externalSurfacePresented);
  EXPECT_DOUBLE_EQ(result.presentedViewport.zoom, 1.0);
}

TEST(DocumentPresenterTest, AFrameTheWorkerSurfaceDoesNotOwnPresentsInTheLiveViewport) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState inactive;
  inactive.viewport = TestViewport();
  inactive.viewport.zoom = 1.0;
  DocumentPresentationFrame frame = TestFrame(inactive);
  frame.viewport.zoom = 3.0;

  const DocumentPresentationResult result = presenter->resolveExternalSurface(frame);

  ASSERT_FALSE(result.externalSurfacePresented);
  EXPECT_DOUBLE_EQ(result.presentedViewport.zoom, 3.0);
}

TEST(DocumentPresenterTest, AHeldPlacementPresentsInTheViewportThatProducedIt) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState surface = ActiveSurface(11);
  surface.viewport.zoom = 1.0;
  std::ignore = presenter->resolveExternalSurface(TestFrame(surface));

  DirectSurfacePresentationState invalidated = surface;
  invalidated.active = false;
  DocumentPresentationFrame frame = TestFrame(invalidated);
  frame.viewport.zoom = 3.0;
  const DocumentPresentationResult result = presenter->resolveExternalSurface(frame);

  ASSERT_TRUE(result.externalSurfacePresented);
  EXPECT_DOUBLE_EQ(result.presentedViewport.zoom, 1.0);
}

TEST(DocumentPresenterTest, PresentationMappingChangeTracksWhatMovesTheDocument) {
  const ViewportState base = TestViewport();

  EXPECT_FALSE(DocumentPresentationMappingChanged(base, base));

  ViewportState zoomed = base;
  zoomed.zoom = 2.0;
  EXPECT_TRUE(DocumentPresentationMappingChanged(base, zoomed));

  ViewportState panned = base;
  panned.panScreenPoint = Vector2d(11.0, 20.0);
  EXPECT_TRUE(DocumentPresentationMappingChanged(base, panned));

  ViewportState refitted = base;
  refitted.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 41.0, 30.0);
  EXPECT_TRUE(DocumentPresentationMappingChanged(base, refitted));

  ViewportState rescaled = base;
  rescaled.devicePixelRatio = 2.0;
  EXPECT_TRUE(DocumentPresentationMappingChanged(base, rescaled));

  // Moving the pane does not move the document inside it.
  ViewportState movedPane = base;
  movedPane.paneOrigin = Vector2d(5.0, 7.0);
  movedPane.paneSize = Vector2d(180.0, 90.0);
  EXPECT_FALSE(DocumentPresentationMappingChanged(base, movedPane));
}

// ---------------------------------------------------------------------------
// Document replacement
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, ADocumentSwapHoldsTheLastAcceptedPlacement) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(11)));
  std::ignore = presenter->presentUnderlay(std::nullopt);

  // Loading a new document invalidates the accepted epoch before any
  // replacement epoch exists.
  DirectSurfacePresentationState invalidated = ActiveSurface(11);
  invalidated.active = false;
  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(invalidated));

  EXPECT_TRUE(result.externalSurfacePresented);
  ASSERT_EQ(sinks.layouts.size(), 2u);
  EXPECT_TRUE(sinks.layouts.back().visible);
  EXPECT_EQ(sinks.layouts.back().frameToken, 11u);
  EXPECT_DOUBLE_EQ(sinks.layouts.back().width, sinks.layouts.front().width);
  // The held frame must not present the framebuffer underlay underneath it.
  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));
  EXPECT_TRUE(sinks.installedPlans().empty());
}

TEST(DocumentPresenterTest, TheSwapHoldExpiresWhenNoReplacementEpochArrives) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(11)));
  std::ignore = presenter->presentUnderlay(std::nullopt);

  DirectSurfacePresentationState invalidated = ActiveSurface(11);
  invalidated.active = false;

  // The hold bridges a document swap; a replacement epoch that never arrives
  // (a parked terminal surface failure, a wedged worker) must not freeze the
  // outgoing document on screen forever. After the frame budget the surface
  // hides and the framebuffer underlay takes the pane again.
  DocumentPresentationResult result;
  for (int frame = 0; frame < 121; ++frame) {
    result = presenter->resolveExternalSurface(TestFrame(invalidated));
    std::ignore = presenter->presentUnderlay(TestPlan());
  }

  EXPECT_FALSE(result.externalSurfacePresented);
  EXPECT_FALSE(sinks.layouts.back().visible);
  EXPECT_FALSE(sinks.installedPlans().empty());
}

TEST(DocumentPresenterTest, TheNewDocumentsFirstEpochReplacesTheHeldPlacement) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(11)));
  DirectSurfacePresentationState invalidated = ActiveSurface(11);
  invalidated.active = false;
  std::ignore = presenter->resolveExternalSurface(TestFrame(invalidated));

  DirectSurfacePresentationState replacement = ActiveSurface(12);
  replacement.rasterViewport.documentRect = Box2d::FromXYWH(0.0, 0.0, 80.0, 20.0);
  std::ignore = presenter->resolveExternalSurface(TestFrame(replacement));

  ASSERT_EQ(sinks.layouts.size(), 3u);
  EXPECT_EQ(sinks.layouts.back().frameToken, 12u);
  EXPECT_DOUBLE_EQ(sinks.layouts.back().width, 80.0);
  EXPECT_DOUBLE_EQ(sinks.layouts.back().height, 20.0);
}

TEST(DocumentPresenterTest, NothingIsHeldBeforeTheFirstAcceptedEpoch) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState inactive;
  const DocumentPresentationResult result = presenter->resolveExternalSurface(TestFrame(inactive));

  EXPECT_FALSE(result.externalSurfacePresented);
  ASSERT_EQ(sinks.layouts.size(), 1u);
  EXPECT_FALSE(sinks.layouts.front().visible);
}

TEST(DocumentPresenterTest, PresentationSuppressionDropsTheHeldPlacement) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(11)));
  // The sample picker owns the pane outright; nothing accepted survives it.
  std::ignore =
      presenter->resolveExternalSurface(TestFrame(ActiveSurface(11), /*presentationSuppressed=*/true));

  DirectSurfacePresentationState invalidated = ActiveSurface(11);
  invalidated.active = false;
  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(invalidated));

  EXPECT_FALSE(result.externalSurfacePresented);
  ASSERT_EQ(sinks.layouts.size(), 3u);
  EXPECT_FALSE(sinks.layouts.back().visible);
}

TEST(DocumentPresenterTest, AnAcceptedEpochScrolledOutOfThePaneIsNotHeld) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(11)));

  // Still accepted, just panned off screen: a legitimate hide, not a swap.
  DirectSurfacePresentationState surface = ActiveSurface(12);
  surface.viewport.panScreenPoint = Vector2d(500.0, 500.0);
  const DocumentPresentationResult result = presenter->resolveExternalSurface(TestFrame(surface));

  EXPECT_FALSE(result.externalSurfacePresented);
  ASSERT_EQ(sinks.layouts.size(), 2u);
  EXPECT_FALSE(sinks.layouts.back().visible);
}

// ---------------------------------------------------------------------------
// Frame-token gating
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, VisibleLayoutForwardsTheAcceptedFrameTokenAndSlot) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(
      TestFrame(ActiveSurface(/*frameCount=*/41, /*surfaceSlot=*/1)));

  ASSERT_EQ(sinks.layouts.size(), 1u);
  EXPECT_TRUE(sinks.layouts.front().visible);
  EXPECT_EQ(sinks.layouts.front().frameToken, 41u);
  EXPECT_EQ(sinks.layouts.front().surfaceSlot, 1);
}

TEST(DocumentPresenterTest, HiddenLayoutStillForwardsTheAcceptedFrameTokenAndSlot) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(
      ActiveSurface(/*frameCount=*/41, /*surfaceSlot=*/1), /*presentationSuppressed=*/true));

  ASSERT_EQ(sinks.layouts.size(), 1u);
  EXPECT_FALSE(sinks.layouts.front().visible);
  EXPECT_EQ(sinks.layouts.front().frameToken, 41u);
  EXPECT_EQ(sinks.layouts.front().surfaceSlot, 1);
  EXPECT_DOUBLE_EQ(sinks.layouts.front().width, 0.0);
  EXPECT_DOUBLE_EQ(sinks.layouts.front().height, 0.0);
}

TEST(DocumentPresenterTest, EveryFrameEmitsExactlyOneLayoutUpdate) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  for (std::uint64_t frameCount = 1; frameCount <= 4; ++frameCount) {
    std::ignore = presenter->resolveExternalSurface(TestFrame(ActiveSurface(frameCount)));
    std::ignore = presenter->presentUnderlay(std::nullopt);
  }

  ASSERT_EQ(sinks.layouts.size(), 4u);
  for (std::size_t index = 0; index < sinks.layouts.size(); ++index) {
    EXPECT_EQ(sinks.layouts[index].frameToken, index + 1u);
  }
}

TEST(DocumentPresenterTest, BakedSelectionChromePassesThroughToTheResult) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  const DocumentPresentationResult result = presenter->resolveExternalSurface(
      TestFrame(ActiveSurface(/*frameCount=*/9, /*surfaceSlot=*/0, /*selectionChromeBaked=*/true)));

  EXPECT_TRUE(result.selectionChromeBaked);
  ASSERT_EQ(sinks.layouts.size(), 1u);
  EXPECT_TRUE(sinks.layouts.front().selectionChromeBaked);
}

TEST(DocumentPresenterTest, BakedChromeIsNotClaimedOnAFrameTheSurfaceDoesNotOwn) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  const DocumentPresentationResult result = presenter->resolveExternalSurface(
      TestFrame(ActiveSurface(/*frameCount=*/9, /*surfaceSlot=*/0, /*selectionChromeBaked=*/true),
                /*presentationSuppressed=*/true));

  EXPECT_FALSE(result.externalSurfacePresented);
  EXPECT_FALSE(result.selectionChromeBaked);
}

// ---------------------------------------------------------------------------
// Underlay dispatch
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, UnderlayPresenterInstallsThePlanItIsGiven) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(DirectSurfacePresentationState{}));
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));

  const std::vector<const FramebufferUnderlayPlan*> installed = sinks.installedPlans();
  ASSERT_EQ(installed.size(), 1u);
  EXPECT_TRUE(installed.front()->suppressDragTargetTiles);
  // The stale plan is cleared before the new one lands, so a frame that decides
  // against the underlay can never inherit last frame's tiles.
  ASSERT_FALSE(sinks.plans.empty());
  EXPECT_FALSE(sinks.plans.front().has_value());
}

TEST(DocumentPresenterTest, UnderlayPresenterClearsWhenThereIsNothingToPresent) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(DirectSurfacePresentationState{}));
  EXPECT_FALSE(presenter->presentUnderlay(std::nullopt));

  EXPECT_TRUE(sinks.installedPlans().empty());
  ASSERT_EQ(sinks.plans.size(), 1u);
  EXPECT_FALSE(sinks.plans.front().has_value());
}

TEST(DocumentPresenterTest, WorkerSurfaceOwningTheFrameSuppressesTheUnderlay) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(ActiveSurface(5)));
  ASSERT_TRUE(result.externalSurfacePresented);

  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));
  EXPECT_TRUE(sinks.installedPlans().empty());
}

TEST(DocumentPresenterTest, WorkerSurfaceFallsBackToTheUnderlayWhenItCannotOwnTheFrame) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState inactive;
  inactive.frameCount = 5;
  const DocumentPresentationResult result = presenter->resolveExternalSurface(TestFrame(inactive));
  ASSERT_FALSE(result.externalSurfacePresented);

  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  EXPECT_EQ(sinks.installedPlans().size(), 1u);
}

// ---------------------------------------------------------------------------
// Frame sequencing invariants
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, UnderlayPresentIsRefusedWithoutAnOpenFrame) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));
  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_TRUE(sinks.plans.empty());
}

TEST(DocumentPresenterTest, ASecondUnderlayPresentInOneFrameIsRefused) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::FramebufferUnderlay, sinks.planSink(), sinks.layoutSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(DirectSurfacePresentationState{}));
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));

  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_EQ(sinks.installedPlans().size(), 1u);
}

TEST(DocumentPresenterTest, WorkerPresenterRefusesASecondUnderlayPresentInOneFrame) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(
      DocumentPresentationTarget::WorkerSurface, sinks.planSink(), sinks.layoutSink());

  DirectSurfacePresentationState inactive;
  std::ignore = presenter->resolveExternalSurface(TestFrame(inactive));
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));

  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_EQ(sinks.installedPlans().size(), 1u);
}

}  // namespace
}  // namespace donner::editor
