#include "donner/editor/ViewportInteractionController.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(ViewportInteractionControllerTest, FrameHistoryTracksLatestAndMax) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(10.0f);
  controller.noteFrameDelta(24.0f);
  controller.noteFrameDelta(12.0f);

  EXPECT_FLOAT_EQ(controller.frameHistory().latest(), 12.0f);
  EXPECT_FLOAT_EQ(controller.frameHistory().max(), 24.0f);
}

TEST(ViewportInteractionControllerTest, FrameHistoryTracksProfilerCostForLatestFrame) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  FrameCostBreakdown cost;
  cost.overlay.captureMs = 1.25;
  cost.overlay.drawMs = 2.5;
  cost.overlay.snapshotMs = 0.75;
  cost.overlay.uploadMs = 0.5;
  cost.compositedUpload.uploadMs = 1.0;
  cost.sourceRopes.layoutMs = 0.25;
  cost.sourceRopes.updateMs = 0.125;
  cost.sourceRopes.drawMs = 0.375;
  controller.frameHistory().setLatestFrameCost(cost);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  const FrameProfilerSample& profiler = controller.frameHistory().profiler[latestIdx];
  EXPECT_FLOAT_EQ(profiler.overlayCaptureMs, 1.25f);
  EXPECT_FLOAT_EQ(profiler.overlayDrawMs, 2.5f);
  EXPECT_FLOAT_EQ(profiler.overlaySnapshotMs, 0.75f);
  EXPECT_FLOAT_EQ(profiler.overlayUploadMs, 0.5f);
  EXPECT_FLOAT_EQ(profiler.compositedUploadMs, 1.0f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeLayoutMs, 0.25f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeUpdateMs, 0.125f);
  EXPECT_FLOAT_EQ(profiler.sourceRopeDrawMs, 0.375f);
  EXPECT_FLOAT_EQ(profiler.totalProfiledMs(), 6.75f);
}

TEST(ViewportInteractionControllerTest, NewFrameClearsProfilerCostSlot) {
  ViewportInteractionController controller;

  controller.noteFrameDelta(16.0f);
  FrameCostBreakdown cost;
  cost.overlay.captureMs = 1.0;
  controller.frameHistory().setLatestFrameCost(cost);
  controller.noteFrameDelta(17.0f);

  const std::size_t latestIdx =
      (controller.frameHistory().writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
  EXPECT_FLOAT_EQ(controller.frameHistory().profiler[latestIdx].totalProfiledMs(), 0.0f);
}

TEST(ViewportInteractionControllerTest, ApplyZoomUsesViewportCenterMath) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  controller.resetToActualSize();

  const Vector2d focal(100.0, 100.0);
  const Vector2d documentBefore = controller.viewport().screenToDocument(focal);
  controller.applyZoom(2.0, focal);
  const Vector2d documentAfter = controller.viewport().screenToDocument(focal);

  EXPECT_NEAR(documentBefore.x, documentAfter.x, 1e-9);
  EXPECT_NEAR(documentBefore.y, documentAfter.y, 1e-9);
}

TEST(ViewportInteractionControllerTest, PanCursorShowsForSpacePanModeOrActivePan) {
  EXPECT_TRUE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/true, /*spaceHeld=*/true,
                                            /*panning=*/false));
  EXPECT_TRUE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/false, /*spaceHeld=*/false,
                                            /*panning=*/true));
  EXPECT_FALSE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/false, /*spaceHeld=*/true,
                                             /*panning=*/false));
  EXPECT_FALSE(ShouldShowRenderPanePanCursor(/*canvasHovered=*/true, /*spaceHeld=*/false,
                                             /*panning=*/false));
}

TEST(ViewportInteractionControllerTest, ConsumeScrollEventsAppliesPanAndZoom) {
  ViewportInteractionController controller;
  controller.viewport().paneOrigin = Vector2d::Zero();
  controller.viewport().paneSize = Vector2d(200.0, 200.0);
  controller.viewport().documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  controller.resetToActualSize();

  std::vector<RenderPaneScrollEvent> panEvents = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 2.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = false,
      },
  };

  controller.consumeScrollEvents(panEvents, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                 /*modalCapturingInput=*/false, /*wheelZoomStep=*/1.1,
                                 /*panPixelsPerScrollUnit=*/10.0);

  EXPECT_TRUE(panEvents.empty());
  EXPECT_NEAR(controller.viewport().panScreenPoint.y, 120.0, 1e-9);

  std::vector<RenderPaneScrollEvent> zoomEvents = {
      RenderPaneScrollEvent{
          .scrollDelta = Vector2d(0.0, 1.0),
          .cursorScreen = Vector2d(100.0, 100.0),
          .zoomModifierHeld = true,
      },
  };

  controller.consumeScrollEvents(zoomEvents, Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                 /*modalCapturingInput=*/false, /*wheelZoomStep=*/1.1,
                                 /*panPixelsPerScrollUnit=*/10.0);

  EXPECT_TRUE(zoomEvents.empty());
  EXPECT_NEAR(controller.viewport().zoom, 1.1, 1e-9);
}

TEST(ViewportInteractionControllerTest, PendingClickBuffersAndClears) {
  ViewportInteractionController controller;
  MouseModifiers modifiers;
  modifiers.shift = true;

  controller.bufferPendingClick(Vector2d(12.0, 34.0), modifiers);
  ASSERT_TRUE(controller.pendingClick().has_value());
  EXPECT_DOUBLE_EQ(controller.pendingClick()->documentPoint.x, 12.0);
  EXPECT_DOUBLE_EQ(controller.pendingClick()->documentPoint.y, 34.0);
  EXPECT_TRUE(controller.pendingClick()->modifiers.shift);

  controller.clearPendingClick();
  EXPECT_FALSE(controller.pendingClick().has_value());
}

}  // namespace
}  // namespace donner::editor
