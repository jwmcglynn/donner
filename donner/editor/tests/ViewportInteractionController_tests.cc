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
