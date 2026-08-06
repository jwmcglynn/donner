#include "donner/editor/RenderPaneGesture.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "donner/editor/PinchEventMonitor.h"
#include "donner/editor/PinchZoomPolicy.h"

namespace donner::editor {
namespace {

::testing::AssertionResult NearVec(const Vector2d& actual, const Vector2d& expected,
                                   double tolerance) {
  const double dx = std::abs(actual.x - expected.x);
  const double dy = std::abs(actual.y - expected.y);
  if (dx > tolerance || dy > tolerance) {
    return ::testing::AssertionFailure()
           << "actual=(" << actual.x << ", " << actual.y << ") expected=(" << expected.x << ", "
           << expected.y << ") |dx|=" << dx << " |dy|=" << dy << " tolerance=" << tolerance;
  }
  return ::testing::AssertionSuccess();
}

#define EXPECT_NEAR_VEC(actual, expected, tolerance) \
  EXPECT_TRUE(NearVec((actual), (expected), (tolerance)))

ViewportState MakeViewport() {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(0.0, 0.0);
  viewport.paneSize = Vector2d(800.0, 600.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);
  viewport.resetTo100Percent();
  return viewport;
}

RenderPaneGestureContext MakeContext() {
  RenderPaneGestureContext context;
  context.paneRect = Box2d::FromXYWH(0.0, 0.0, 800.0, 600.0);
  return context;
}

TEST(RenderPaneGestureTest, PlainScrollInsidePaneBecomesPan) {
  const RenderPaneScrollEvent event{
      .scrollDelta = Vector2d(1.5, -2.0),
      .cursorScreen = Vector2d(400.0, 300.0),
      .zoomModifierHeld = false,
  };

  const auto action = ClassifyRenderPaneScrollGesture(event, MakeContext(), /*wheelZoomStep=*/1.1,
                                                      /*panPixelsPerScrollUnit=*/10.0);
  ASSERT_TRUE(action.has_value());
  EXPECT_EQ(action->type, RenderPaneGestureActionType::Pan);
  EXPECT_NEAR_VEC(action->panScreenDelta, Vector2d(15.0, -20.0), 1e-9);
}

TEST(RenderPaneGestureTest, ModifiedVerticalScrollBecomesZoomAroundCursor) {
  const RenderPaneScrollEvent event{
      .scrollDelta = Vector2d(0.0, 2.0),
      .cursorScreen = Vector2d(250.0, 125.0),
      .zoomModifierHeld = true,
  };

  const auto action = ClassifyRenderPaneScrollGesture(event, MakeContext(), /*wheelZoomStep=*/1.1,
                                                      /*panPixelsPerScrollUnit=*/10.0);
  ASSERT_TRUE(action.has_value());
  EXPECT_EQ(action->type, RenderPaneGestureActionType::Zoom);
  EXPECT_NEAR(action->zoomFactor, 1.21, 1e-12);
  EXPECT_NEAR_VEC(action->focalScreen, Vector2d(250.0, 125.0), 1e-9);
}

TEST(RenderPaneGestureTest, PinchMagnificationMapsToEquivalentZoomInScrollDelta) {
  const double scrollDelta = PinchMagnificationToScrollDelta(/*magnification=*/0.2,
                                                             /*wheelZoomStep=*/1.1);

  EXPECT_NEAR(std::pow(1.1, scrollDelta), 1.2, 1e-12);
}

TEST(RenderPaneGestureTest, PinchMagnificationMapsToEquivalentZoomOutScrollDelta) {
  const double scrollDelta = PinchMagnificationToScrollDelta(/*magnification=*/-0.2,
                                                             /*wheelZoomStep=*/1.1);

  EXPECT_NEAR(std::pow(1.1, scrollDelta), 0.8, 1e-12);
}

TEST(RenderPaneGestureTest, PinchMagnificationRoundTripsToTheExactZoomFactor) {
  // The whole pinch policy rests on this identity: a Cocoa magnification of `m`
  // must zoom the document by exactly `1 + m` after the classifier re-applies
  // the wheel-zoom step. Sweep the useful magnification range rather than
  // spot-checking two values, so any change to either half of the conversion
  // fails here.
  for (double magnification = -0.5; magnification <= 2.0 + 1e-12; magnification += 0.01) {
    const double scrollDelta = PinchMagnificationToScrollDelta(magnification, kWheelZoomStep);
    EXPECT_NEAR(std::pow(kWheelZoomStep, scrollDelta), 1.0 + magnification, 1e-9)
        << "magnification=" << magnification;
  }
}

TEST(RenderPaneGestureTest, PinchWheelDeltaPerLnScaleMatchesTheClassifierStep) {
  // Pins the browser bridge constant to its derivation. The Wasm GLFW port
  // converts DOM_DELTA_PIXEL wheel input with `yoffset = -deltaY / 100`, so a
  // bridge emitting `deltaY = -K * ln(scale)` zooms by exactly `scale` only
  // when `K == 100 / ln(kWheelZoomStep)`.
  EXPECT_NEAR(PinchWheelDeltaPerLnScale(), 100.0 / std::log(1.1), 1e-12);
  EXPECT_NEAR(PinchWheelDeltaPerLnScale(), 1049.205868725706, 1e-9);

  // The literal fallback baked into editor-bootstrap.js, for the window between
  // page load and Wasm runtime initialization, must stay within rounding
  // distance of the published value.
  EXPECT_NEAR(PinchWheelDeltaPerLnScale(), 1049.2059, 1e-3);
}

TEST(RenderPaneGestureTest, BrowserPinchWheelDeltaZoomsByTheGestureScale) {
  // End-to-end policy check across the browser path, with the emscripten-glfw
  // pixel-to-scroll-unit conversion modeled inline.
  for (const double scale : {0.5, 0.8, 1.25, 2.0, 3.0}) {
    const double bridgeDeltaY = -std::log(scale) * PinchWheelDeltaPerLnScale();
    const double glfwYOffset = -bridgeDeltaY / kWasmWheelPixelsPerScrollUnit;
    const RenderPaneScrollEvent event{
        .scrollDelta = Vector2d(0.0, glfwYOffset),
        .cursorScreen = Vector2d(400.0, 300.0),
        .zoomModifierHeld = true,
    };

    const auto action = ClassifyRenderPaneScrollGesture(event, MakeContext(), kWheelZoomStep,
                                                        /*panPixelsPerScrollUnit=*/10.0);
    ASSERT_TRUE(action.has_value()) << "scale=" << scale;
    EXPECT_EQ(action->type, RenderPaneGestureActionType::Zoom);
    EXPECT_NEAR(action->zoomFactor, scale, 1e-9) << "scale=" << scale;
  }
}

TEST(RenderPaneGestureTest, DegeneratePinchMagnificationIsIgnored) {
  EXPECT_DOUBLE_EQ(PinchMagnificationToScrollDelta(/*magnification=*/-1.0,
                                                   /*wheelZoomStep=*/1.1),
                   0.0);
}

TEST(RenderPaneGestureTest, ScrollOutsidePaneIsIgnored) {
  const RenderPaneScrollEvent event{
      .scrollDelta = Vector2d(0.0, 1.0),
      .cursorScreen = Vector2d(900.0, 700.0),
      .zoomModifierHeld = false,
  };

  EXPECT_FALSE(ClassifyRenderPaneScrollGesture(event, MakeContext(), /*wheelZoomStep=*/1.1,
                                               /*panPixelsPerScrollUnit=*/10.0)
                   .has_value());
}

TEST(RenderPaneGestureTest, ModifiedHorizontalOnlyScrollIsIgnored) {
  const RenderPaneScrollEvent event{
      .scrollDelta = Vector2d(1.0, 0.0),
      .cursorScreen = Vector2d(400.0, 300.0),
      .zoomModifierHeld = true,
  };

  EXPECT_FALSE(ClassifyRenderPaneScrollGesture(event, MakeContext(), /*wheelZoomStep=*/1.1,
                                               /*panPixelsPerScrollUnit=*/10.0)
                   .has_value());
}

TEST(RenderPaneGestureTest, MouseDragPanBlocksWheelGestures) {
  RenderPaneGestureContext context = MakeContext();
  context.mouseDragPanActive = true;
  const RenderPaneScrollEvent event{
      .scrollDelta = Vector2d(0.0, 1.0),
      .cursorScreen = Vector2d(400.0, 300.0),
      .zoomModifierHeld = false,
  };

  EXPECT_FALSE(ClassifyRenderPaneScrollGesture(event, context, /*wheelZoomStep=*/1.1,
                                               /*panPixelsPerScrollUnit=*/10.0)
                   .has_value());
}

TEST(RenderPaneGestureTest, GestureSequenceMutatesViewportToExpectedEndState) {
  ViewportState viewport = MakeViewport();
  const RenderPaneGestureContext context = MakeContext();
  const RenderPaneScrollEvent panEvent{
      .scrollDelta = Vector2d(3.0, -2.0),
      .cursorScreen = Vector2d(420.0, 290.0),
      .zoomModifierHeld = false,
  };
  const auto panAction = ClassifyRenderPaneScrollGesture(panEvent, context, /*wheelZoomStep=*/1.1,
                                                         /*panPixelsPerScrollUnit=*/10.0);
  ASSERT_TRUE(panAction.has_value());
  ApplyRenderPaneGesture(viewport, *panAction);

  const RenderPaneScrollEvent zoomEvent{
      .scrollDelta = Vector2d(0.0, 2.0),
      .cursorScreen = Vector2d(420.0, 290.0),
      .zoomModifierHeld = true,
  };
  const Vector2d focalScreen = zoomEvent.cursorScreen;
  const Vector2d focalDocBeforeZoom = viewport.screenToDocument(focalScreen);
  const auto zoomAction = ClassifyRenderPaneScrollGesture(zoomEvent, context, /*wheelZoomStep=*/1.1,
                                                          /*panPixelsPerScrollUnit=*/10.0);
  ASSERT_TRUE(zoomAction.has_value());
  ApplyRenderPaneGesture(viewport, *zoomAction);

  EXPECT_NEAR(viewport.zoom, 1.21, 1e-12);
  EXPECT_NEAR_VEC(viewport.panScreenPoint, focalScreen, 1e-9);
  EXPECT_NEAR_VEC(viewport.documentToScreen(focalDocBeforeZoom), focalScreen, 1e-9);
}

TEST(RenderPaneGestureTest, CanvasOwnsScrollEventInsideCaptureRect) {
  const std::optional<Box2d> captureRect = Box2d::FromXYWH(100.0, 50.0, 800.0, 600.0);

  // Inside the render pane: the canvas owns the event (UI must not scroll).
  EXPECT_TRUE(CanvasOwnsScrollEvent(captureRect, Vector2d(400.0, 300.0)));

  // Outside the pane (e.g. over the source pane or sidebars): the UI layer
  // keeps the event.
  EXPECT_FALSE(CanvasOwnsScrollEvent(captureRect, Vector2d(50.0, 300.0)));
  EXPECT_FALSE(CanvasOwnsScrollEvent(captureRect, Vector2d(950.0, 300.0)));
  EXPECT_FALSE(CanvasOwnsScrollEvent(captureRect, Vector2d(400.0, 20.0)));
}

TEST(RenderPaneGestureTest, CanvasNeverOwnsScrollEventWhileCaptureDisabled) {
  // Capture disabled (popup/modal open, or no pane yet): everything goes to
  // the UI layer, even inside where the pane sits.
  EXPECT_FALSE(CanvasOwnsScrollEvent(std::nullopt, Vector2d(400.0, 300.0)));
}

}  // namespace
}  // namespace donner::editor
