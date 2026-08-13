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

DocumentPresentationFrame TestFrame(bool presentationSuppressed = false) {
  return DocumentPresentationFrame{
      .viewport = TestViewport(),
      .paneRect = TestPaneRect(),
      .presentationSuppressed = presentationSuppressed,
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

  // There is one presentation target on every platform, and it is the plan
  // sink: nothing lands outside the window framebuffer, so nothing else can
  // observe this frame.
  std::ignore = presenter->resolveExternalSurface(TestFrame());
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  ASSERT_EQ(sinks.installedPlans().size(), 1u);
  EXPECT_TRUE(sinks.installedPlans().front()->suppressDragTargetTiles);
}

// ---------------------------------------------------------------------------
// Presented viewport
// ---------------------------------------------------------------------------

TEST(DocumentPresenterTest, TheUnderlayPresentsInTheLiveViewport) {
  RecordingSinks sinks;
  const std::unique_ptr<DocumentPresenter> presenter = MakeDocumentPresenter(sinks.planSink());

  DocumentPresentationFrame frame = TestFrame();
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

  std::ignore = presenter->resolveExternalSurface(TestFrame());
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

  std::ignore = presenter->resolveExternalSurface(TestFrame());
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

  std::ignore = presenter->resolveExternalSurface(TestFrame());
  EXPECT_TRUE(presenter->presentUnderlay(TestPlan()));
  EXPECT_FALSE(presenter->presentUnderlay(TestPlan()));

  EXPECT_EQ(presenter->refusedUnderlayPresentCount(), 1u);
  EXPECT_EQ(sinks.installedPlans().size(), 1u);
}

}  // namespace
}  // namespace donner::editor
