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
  surface.frameCount = frameCount;
  surface.surfaceSlot = surfaceSlot;
  surface.selectionChromeBaked = selectionChromeBaked;
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
  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
  // Slide the document up and left so it overhangs the pane's top-left corner.
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
  DocumentPresentationFrame frame = TestFrame(ActiveSurface(3));
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
