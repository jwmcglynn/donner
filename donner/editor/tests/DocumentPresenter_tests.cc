#include "donner/editor/DocumentPresenter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
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

DocumentPresentationFrame TestFrame(DirectSurfacePresentationState surface,
                                    bool presentationSuppressed = false) {
  return DocumentPresentationFrame{
      .viewport = TestViewport(),
      .paneRect = TestPaneRect(),
      .presentationSuppressed = presentationSuppressed,
      .workerSurface = std::move(surface),
  };
}

/// Records everything the presenter under test hands to its sink.
struct RecordingSinks {
  std::vector<std::optional<FramebufferUnderlayPlan>> plans;

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

TEST(DocumentPresenterTest, EveryBuildPresentsThroughTheFramebufferUnderlay) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  EXPECT_FALSE(presenter->presentsToExternalSurface());

  // Even with an accepted worker epoch in the frame, nothing presents outside
  // the window framebuffer on any build.
  const DocumentPresentationResult result =
      presenter->resolveExternalSurface(TestFrame(ActiveSurface(7)));
  EXPECT_FALSE(result.documentPresentedDirectly);
  EXPECT_FALSE(result.externalSurfacePresented);
  EXPECT_FALSE(result.selectionChromeBaked);
}

// ---------------------------------------------------------------------------
// The live-placement coverage clamp
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, CoverageClampAcceptsAGrowingPlacement) {
  const Box2d epochRect = Box2d::FromXYWH(-32.0, -32.0, 264.0, 164.0);
  const Box2d liveRect = Box2d::FromXYWH(-48.0, -48.0, 396.0, 246.0);

  EXPECT_TRUE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

TEST(DocumentPresenterTest, CoverageClampRejectsAPlacementThatUncoversPaneArea) {
  const Box2d epochRect = Box2d::FromXYWH(-32.0, -32.0, 264.0, 164.0);
  // Half the scale: the surface no longer reaches the pane's right/bottom edge.
  const Box2d liveRect = Box2d::FromXYWH(-16.0, -16.0, 132.0, 82.0);

  EXPECT_FALSE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

TEST(DocumentPresenterTest, CoverageClampIgnoresAreaOutsideThePane) {
  // The epoch overhung the pane by 32px on every side. The live placement gives
  // that overhang up but still covers every pane pixel, so nothing visible is
  // lost.
  const Box2d epochRect = Box2d::FromXYWH(-32.0, -32.0, 264.0, 164.0);
  const Box2d liveRect = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);

  EXPECT_TRUE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

TEST(DocumentPresenterTest, CoverageClampAcceptsAnEpochThatCoveredNoPaneArea) {
  const Box2d epochRect = Box2d::FromXYWH(500.0, 500.0, 40.0, 30.0);
  const Box2d liveRect = Box2d::FromXYWH(900.0, 900.0, 1.0, 1.0);

  EXPECT_TRUE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

TEST(DocumentPresenterTest, CoverageClampToleratesSubPixelTransformDrift) {
  // The two rects come out of separate floating-point transform chains, so an
  // exact containment test would reject placements identical to under a pixel.
  const Box2d epochRect = Box2d::FromXYWH(10.0, 20.0, 40.0, 30.0);
  const Box2d liveRect = Box2d::FromXYWH(10.0 + 1e-9, 20.0 + 1e-9, 40.0, 30.0);

  EXPECT_TRUE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

TEST(DocumentPresenterTest, CoverageClampRejectsANonFiniteLivePlacement) {
  const Box2d epochRect = Box2d::FromXYWH(10.0, 20.0, 40.0, 30.0);
  const Box2d liveRect(Vector2d(std::numeric_limits<double>::quiet_NaN(), 0.0),
                       Vector2d(400.0, 400.0));

  EXPECT_FALSE(LivePlacementCoversEpochCoverage(TestPaneRect(), epochRect, liveRect));
}

// ---------------------------------------------------------------------------
// Presentation-only frame admission
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, APresentationOnlyFrameMayPlaceAnEpochThatDoesNotMove) {
  const ViewportState presented = TestViewport();
  EXPECT_TRUE(PresentationOnlyFrameMayPlaceEpoch(presented, presented));
}

TEST(DocumentPresenterTest, APresentationOnlyFrameRefusesAnEpochThatMoves) {
  const ViewportState presented = TestViewport();
  ViewportState candidate = presented;
  candidate.zoom = 1.25;

  EXPECT_FALSE(PresentationOnlyFrameMayPlaceEpoch(presented, candidate));
}

TEST(DocumentPresenterTest, APresentationOnlyFrameMayPlaceWhenNoFullFrameHasPresented) {
  EXPECT_TRUE(PresentationOnlyFrameMayPlaceEpoch(std::nullopt, TestViewport()));
}

TEST(DocumentPresenterTest, APresentationOnlyFrameIgnoresPaneGeometryAlone) {
  // Pane origin and size do not by themselves move the document within the
  // pane, and this path reuses the last full frame's pane rect anyway.
  const ViewportState presented = TestViewport();
  ViewportState candidate = presented;
  candidate.paneOrigin = Vector2d(4.0, 4.0);

  EXPECT_TRUE(PresentationOnlyFrameMayPlaceEpoch(presented, candidate));
}

// ---------------------------------------------------------------------------
// Presented viewport
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, TheUnderlayPresentsInTheLiveViewport) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
  frame.viewport.zoom = 2.5;

  const DocumentPresentationResult result = presenter->resolveExternalSurface(frame);

  EXPECT_DOUBLE_EQ(result.presentedViewport.zoom, 2.5);
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
// Underlay dispatch
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, UnderlayPresenterInstallsThePlanItIsGiven) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

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
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(DirectSurfacePresentationState{}));
  EXPECT_FALSE(presenter->presentUnderlay(std::nullopt));

  EXPECT_TRUE(sinks.installedPlans().empty());
  ASSERT_EQ(sinks.plans.size(), 1u);
  EXPECT_FALSE(sinks.plans.front().has_value());
}

// ---------------------------------------------------------------------------
// Frame sequencing invariants
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, UnderlayPresentIsRefusedWithoutAnOpenFrame) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));
  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_TRUE(sinks.plans.empty());
}

TEST(DocumentPresenterTest, ASecondUnderlayPresentInOneFrameIsRefused) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  std::ignore = presenter->resolveExternalSurface(TestFrame(DirectSurfacePresentationState{}));
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));

  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_EQ(sinks.installedPlans().size(), 1u);
}

}  // namespace
}  // namespace donner::editor
