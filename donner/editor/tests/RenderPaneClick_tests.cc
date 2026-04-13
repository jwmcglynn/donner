#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {
namespace {

// Two non-overlapping rects in a 200x200 viewBox. r1 is in the
// top-left quadrant, r2 in the bottom-right. The test sets the
// viewport up at various zoom/pan/dpr combinations and asserts that
// clicking at the screen position of each rect's center hits the
// expected element id.
constexpr std::string_view kTwoRectSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
            <rect id="r1" x="20"  y="20"  width="40" height="40" fill="red"/>
            <rect id="r2" x="140" y="140" width="40" height="40" fill="blue"/>
          </svg>)svg";

ViewportState MakeViewportFor(EditorApp& app, Vector2d paneOrigin, Vector2d paneSize,
                              double dpr = 1.0) {
  ViewportState v;
  v.paneOrigin = paneOrigin;
  v.paneSize = paneSize;
  v.devicePixelRatio = dpr;
  v.documentViewBox = *app.document().document().svgElement().viewBox();
  v.resetTo100Percent();
  return v;
}

// At any zoom and any non-zero pan, clicking the *screen* position of
// an element's center must hit-test to that element. This is the
// invariant the user reported broken: "mouse clicking is not right
// when zoomed". Parameterized over zoom/pan to catch any regression
// where the math works at zoom=1 but drifts at zoom=4.
TEST(RenderPaneClickTest, ClickAtElementCenterHitsElementAtAnyZoom) {
  for (double zoom : {0.5, 1.0, 2.0, 4.0}) {
    for (Vector2d pan : {Vector2d(0.0, 0.0), Vector2d(75.0, -40.0),
                         Vector2d(-100.0, 30.0)}) {
      EditorApp app;
      ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
      ViewportState v =
          MakeViewportFor(app, Vector2d(0.0, 0.0), Vector2d(800.0, 600.0));

      // Apply zoom around the pane center, then pan.
      v.zoomAround(zoom, v.paneCenter());
      v.panBy(pan);

      // Element centers in document coordinates.
      const Vector2d r1Center(40.0, 40.0);    // 20 + 40/2
      const Vector2d r2Center(160.0, 160.0);  // 140 + 40/2

      // Map to screen, then back through the same viewport — the
      // round-trip should land on the same element.
      const Vector2d r1Screen = v.documentToScreen(r1Center);
      const auto hitR1 = app.hitTest(v.screenToDocument(r1Screen));
      ASSERT_TRUE(hitR1.has_value())
          << "  zoom=" << zoom << " pan=(" << pan.x << "," << pan.y << ")";
      EXPECT_EQ(hitR1->id(), "r1")
          << "  zoom=" << zoom << " pan=(" << pan.x << "," << pan.y << ")";

      const Vector2d r2Screen = v.documentToScreen(r2Center);
      const auto hitR2 = app.hitTest(v.screenToDocument(r2Screen));
      ASSERT_TRUE(hitR2.has_value());
      EXPECT_EQ(hitR2->id(), "r2");
    }
  }
}

// DPR 2x and 3x must not move click positions on the screen — DPR
// only affects rasterization fidelity, not coordinate math, since
// every screen pixel value is in *logical* pixels.
TEST(RenderPaneClickTest, ClickAtElementCenterHitsElementAtAnyDpr) {
  for (double dpr : {1.0, 1.5, 2.0, 3.0}) {
    EditorApp app;
    ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
    ViewportState v =
        MakeViewportFor(app, Vector2d(0.0, 0.0), Vector2d(800.0, 600.0), dpr);
    v.zoomAround(2.5, v.paneCenter());

    const Vector2d r1Screen = v.documentToScreen(Vector2d(40.0, 40.0));
    const auto hit = app.hitTest(v.screenToDocument(r1Screen));
    ASSERT_TRUE(hit.has_value()) << "  dpr=" << dpr;
    EXPECT_EQ(hit->id(), "r1") << "  dpr=" << dpr;
  }
}

// `desiredCanvasSize` should grow with both zoom and DPR, capped at
// `kMaxCanvasDim`. This is what the editor pipes into
// `setCanvasSize` to drive the renderer's bitmap resolution.
TEST(RenderPaneClickTest, DesiredCanvasSizeIsViewBoxScaledByZoomAndDpr) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d(0.0, 0.0), Vector2d(800.0, 600.0), /*dpr=*/2.0);
  v.zoomAround(3.0, v.paneCenter());

  const Vector2i size = v.desiredCanvasSize();
  // 200 viewBox * 3 zoom * 2 dpr = 1200 device pixels per axis.
  EXPECT_EQ(size.x, 1200);
  EXPECT_EQ(size.y, 1200);
}

// Changing DPR from 1× to 2× must NOT change the on-screen layout
// rectangle or click math — DPR only affects how many device pixels
// the rasterizer produces per logical pixel. The key invariant: the
// `imageScreenRect()` is the same at any DPR for the same zoom and
// pane size, and the same logical-pixel click maps to the same
// document point.
//
// This is the regression test for "Retina users see clicks land on
// the wrong element" — the bug class where DPR sneaks into the
// screen-coordinate path and offsets click positions by the device-
// pixel ratio.
TEST(RenderPaneClickTest, DprChangeDoesNotMoveOnScreenRectOrClicks) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v1 =
      MakeViewportFor(app, Vector2d(50.0, 30.0), Vector2d(800.0, 600.0), /*dpr=*/1.0);
  v1.zoomAround(2.0, v1.paneCenter());
  v1.panBy(Vector2d(40.0, -25.0));

  ViewportState v2 = v1;
  v2.devicePixelRatio = 2.0;

  // Same on-screen rectangle at 1× and 2×.
  const Box2d rect1 = v1.imageScreenRect();
  const Box2d rect2 = v2.imageScreenRect();
  EXPECT_DOUBLE_EQ(rect1.topLeft.x, rect2.topLeft.x);
  EXPECT_DOUBLE_EQ(rect1.topLeft.y, rect2.topLeft.y);
  EXPECT_DOUBLE_EQ(rect1.bottomRight.x, rect2.bottomRight.x);
  EXPECT_DOUBLE_EQ(rect1.bottomRight.y, rect2.bottomRight.y);

  // A click at the center of r1 in screen space hits r1 at both
  // DPRs. (At any DPR the screen position is unchanged because we
  // operate in logical pixels; only the underlying canvas grows.)
  const Vector2d r1Screen = v1.documentToScreen(Vector2d(40.0, 40.0));
  EXPECT_TRUE(app.hitTest(v1.screenToDocument(r1Screen)).has_value());
  EXPECT_TRUE(app.hitTest(v2.screenToDocument(r1Screen)).has_value());
  EXPECT_EQ(app.hitTest(v1.screenToDocument(r1Screen))->id(), "r1");
  EXPECT_EQ(app.hitTest(v2.screenToDocument(r1Screen))->id(), "r1");
}

// At 2× DPR the canvas should be exactly twice the pixel dimensions
// of the 1× version for the same logical viewport state — the
// visible image is the same size on screen, the underlying bitmap
// just has 4× the pixels.
TEST(RenderPaneClickTest, DprDoublesCanvasPixelsButNotScreenRect) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v1x =
      MakeViewportFor(app, Vector2d::Zero(), Vector2d(800.0, 600.0), /*dpr=*/1.0);
  ViewportState v2x = v1x;
  v2x.devicePixelRatio = 2.0;

  EXPECT_EQ(v2x.desiredCanvasSize().x, v1x.desiredCanvasSize().x * 2);
  EXPECT_EQ(v2x.desiredCanvasSize().y, v1x.desiredCanvasSize().y * 2);

  // On-screen image rect is unchanged.
  EXPECT_DOUBLE_EQ(v1x.imageScreenRect().width(), v2x.imageScreenRect().width());
  EXPECT_DOUBLE_EQ(v1x.imageScreenRect().height(), v2x.imageScreenRect().height());
}

// A drag at high DPR translates the element by document-space
// delta, not device-pixel delta. Same property as the
// `DragMovesElementByDocDeltaUnderZoom` test, but with DPR=2 to pin
// that DPR doesn't accidentally sneak into the drag's coordinate
// transform.
TEST(RenderPaneClickTest, DragMovesElementByDocDeltaUnderHighDpr) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d::Zero(), Vector2d(800.0, 600.0), /*dpr=*/2.0);
  v.zoomAround(2.0, v.paneCenter());

  SelectTool tool;
  const Vector2d startScreen = v.documentToScreen(Vector2d(40.0, 40.0));
  tool.onMouseDown(app, v.screenToDocument(startScreen), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());

  auto rect = app.selectedElement()->cast<svg::SVGGraphicsElement>();
  const Transform2d startTransform = rect.transform();

  // Drag 100 *logical* screen pixels right at zoom=2 and dpr=2 →
  // doc delta = 100 / zoom = 50 doc units. DPR must not factor in.
  const Vector2d targetScreen = startScreen + Vector2d(100.0, 0.0);
  tool.onMouseMove(app, v.screenToDocument(targetScreen), /*buttonHeld=*/true);
  app.flushFrame();

  const Transform2d after = rect.transform();
  EXPECT_NEAR(after.data[4] - startTransform.data[4], 50.0, 1e-6);
  EXPECT_NEAR(after.data[5] - startTransform.data[5], 0.0, 1e-6);
}

// Drag smoothness: when SelectTool receives a sequence of N
// equally-spaced `onMouseMove` events, the element's transform
// should land at the corresponding intermediate translation each
// step, with no skipped or duplicated positions. This is the
// regression test for "drags should be smooth".
TEST(RenderPaneClickTest, DragMovesElementByCursorDelta) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d(0.0, 0.0), Vector2d(800.0, 600.0));

  // Click on r1's center to start the drag.
  SelectTool tool;
  const Vector2d startScreen = v.documentToScreen(Vector2d(40.0, 40.0));
  tool.onMouseDown(app, v.screenToDocument(startScreen), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());
  ASSERT_EQ(app.selectedElement()->id(), "r1");

  // Capture the element's starting transform so we can diff against
  // it on each step. r1 has no transform attribute, so this is
  // identity.
  auto rect = app.selectedElement()->cast<svg::SVGGraphicsElement>();
  const Transform2d startTransform = rect.transform();

  // Drag the cursor 100 screen pixels right and 50 down, in 10
  // equal steps. Each step's resulting translation should equal
  // the cumulative cursor delta in document space.
  constexpr int kSteps = 10;
  const Vector2d totalScreenDelta(100.0, 50.0);
  for (int i = 1; i <= kSteps; ++i) {
    const Vector2d screenAtStep =
        startScreen + totalScreenDelta * (static_cast<double>(i) / kSteps);
    tool.onMouseMove(app, v.screenToDocument(screenAtStep), /*buttonHeld=*/true);
    // Drain the queued SetTransformCommand so the element's transform
    // reflects the move.
    app.flushFrame();

    const Transform2d nowTransform = rect.transform();
    // The applied translation should be the cumulative document-space
    // cursor delta. At zoom=1 dpr=1, doc and screen scales agree, so
    // this is just the cursor delta divided by 1.
    const Vector2d expectedDocDelta =
        v.screenToDocument(screenAtStep) - v.screenToDocument(startScreen);
    EXPECT_NEAR(nowTransform.data[4] - startTransform.data[4], expectedDocDelta.x, 1e-6)
        << "  step=" << i << " of " << kSteps;
    EXPECT_NEAR(nowTransform.data[5] - startTransform.data[5], expectedDocDelta.y, 1e-6)
        << "  step=" << i << " of " << kSteps;
  }

  tool.onMouseUp(app, v.screenToDocument(startScreen + totalScreenDelta));
}

// Regression for "clicks dropped during initial render" reported on
// 2026-04-13. main.cc captures the click position *immediately* on
// the press frame and stores it as a `PendingClick`, then dispatches
// it on the next frame the async render worker is idle. This test
// mimics that pattern: simulate a click while "busy", then a frame
// later when "idle" the deferred click gets handed to SelectTool and
// produces the expected selection.
TEST(RenderPaneClickTest, BufferedClickDispatchesWhenWorkerIdle) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d::Zero(), Vector2d(800.0, 600.0));

  struct PendingClick {
    Vector2d documentPoint;
    MouseModifiers modifiers;
  };
  std::optional<PendingClick> pendingClick;

  // Frame 1: simulated worker is "busy", click captured to buffer.
  bool workerBusy = true;
  const Vector2d r1Screen = v.documentToScreen(Vector2d(40.0, 40.0));
  pendingClick = PendingClick{v.screenToDocument(r1Screen), MouseModifiers{}};

  SelectTool tool;
  // Don't dispatch yet — worker is busy.
  if (pendingClick.has_value() && !workerBusy) {
    tool.onMouseDown(app, pendingClick->documentPoint, pendingClick->modifiers);
    pendingClick.reset();
  }
  EXPECT_FALSE(app.hasSelection()) << "Click should not fire while worker is busy";
  EXPECT_TRUE(pendingClick.has_value()) << "Click should still be buffered";

  // Frame 2: worker becomes idle. Pending click dispatches.
  workerBusy = false;
  if (pendingClick.has_value() && !workerBusy) {
    tool.onMouseDown(app, pendingClick->documentPoint, pendingClick->modifiers);
    pendingClick.reset();
  }
  EXPECT_TRUE(app.hasSelection())
      << "Buffered click should dispatch on the first idle frame";
  EXPECT_EQ(app.selectedElement()->id(), "r1");
  EXPECT_FALSE(pendingClick.has_value());
}

// Regression for "I can't drag shapes" reported on 2026-04-13:
// mimics main.cc's exact frame-by-frame call pattern. On the FIRST
// click frame, main.cc fires `onMouseDown` and *then* immediately
// fires `onMouseMove` (because `ImGui::IsMouseDown` is also true on
// the press frame). The next frame, `IsMouseClicked` is false but
// `IsMouseDown` stays true, so `onMouseMove` keeps firing. This test
// drives that exact sequence and asserts the element actually moves.
TEST(RenderPaneClickTest, MainLoopClickDragSequenceMovesElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d::Zero(), Vector2d(800.0, 600.0));

  SelectTool tool;
  const Vector2d r1ScreenStart = v.documentToScreen(Vector2d(40.0, 40.0));

  // ---- Frame N: IsMouseClicked = true, IsMouseDown = true ----
  // main.cc fires onMouseDown, then immediately fires onMouseMove
  // with the same point.
  tool.onMouseDown(app, v.screenToDocument(r1ScreenStart), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());
  ASSERT_EQ(app.selectedElement()->id(), "r1");
  ASSERT_TRUE(tool.isDragging());
  tool.onMouseMove(app, v.screenToDocument(r1ScreenStart), /*buttonHeld=*/true);

  // Next frame top: flushFrame applies the queued (no-op) command.
  app.flushFrame();
  auto rect = app.selectedElement()->cast<svg::SVGGraphicsElement>();
  const Transform2d startTransform = rect.transform();

  // ---- Frame N+1: IsMouseClicked = false, IsMouseDown = true ----
  // mouse moves by (40, 0).
  const Vector2d screenAt1 = r1ScreenStart + Vector2d(40.0, 0.0);
  tool.onMouseMove(app, v.screenToDocument(screenAt1), /*buttonHeld=*/true);
  app.flushFrame();
  EXPECT_NEAR(rect.transform().data[4] - startTransform.data[4], 40.0, 1e-6)
      << "Element didn't move on the first post-click drag frame";

  // ---- Frame N+2: another move, this time by (40, 30). ----
  const Vector2d screenAt2 = r1ScreenStart + Vector2d(40.0, 30.0);
  tool.onMouseMove(app, v.screenToDocument(screenAt2), /*buttonHeld=*/true);
  app.flushFrame();
  EXPECT_NEAR(rect.transform().data[4] - startTransform.data[4], 40.0, 1e-6);
  EXPECT_NEAR(rect.transform().data[5] - startTransform.data[5], 30.0, 1e-6);

  // ---- Frame N+3: IsMouseReleased = true ----
  tool.onMouseUp(app, v.screenToDocument(screenAt2));
  EXPECT_FALSE(tool.isDragging());
  EXPECT_TRUE(tool.consumeDragCompleted())
      << "drag-completed flag should fire so the writeback path runs";
}

// Regression for "marquee dragging up doesn't select properly".
// Drives the SelectTool through a marquee drag in the upward
// direction (start point below-and-right of the elements, end point
// above-and-left) and asserts that BOTH elements get selected.
TEST(RenderPaneClickTest, MarqueeDragUpwardSelectsIntersectingElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d::Zero(), Vector2d(800.0, 600.0));

  SelectTool tool;
  // r1 lives at (20..60, 20..60), r2 at (140..180, 140..180) in the
  // 200x200 viewBox. Start below-right of r2, end above-left of r1.
  const Vector2d screenStart = v.documentToScreen(Vector2d(190.0, 190.0));
  const Vector2d screenEnd = v.documentToScreen(Vector2d(10.0, 10.0));

  tool.onMouseDown(app, v.screenToDocument(screenStart), MouseModifiers{});
  ASSERT_TRUE(tool.isMarqueeing());

  // Drag step-by-step *up and to the left* — same direction the
  // user reported broken in the editor.
  for (int step = 1; step <= 5; ++step) {
    const double t = static_cast<double>(step) / 5.0;
    const Vector2d midScreen =
        screenStart + (screenEnd - screenStart) * t;
    tool.onMouseMove(app, v.screenToDocument(midScreen), /*buttonHeld=*/true);
    ASSERT_TRUE(tool.marqueeRect().has_value());
    // The marquee rect is always normalized so `topLeft` has the
    // smaller coordinates, even though we're dragging up-left.
    EXPECT_LE(tool.marqueeRect()->topLeft.x, tool.marqueeRect()->bottomRight.x);
    EXPECT_LE(tool.marqueeRect()->topLeft.y, tool.marqueeRect()->bottomRight.y);
  }

  tool.onMouseUp(app, v.screenToDocument(screenEnd));
  EXPECT_FALSE(tool.isMarqueeing());
  EXPECT_FALSE(tool.marqueeRect().has_value());
  EXPECT_EQ(app.selectedElements().size(), 2u)
      << "Upward marquee should select both elements just like a downward marquee does";
}

// Same test but at a non-trivial zoom: a 100-pixel screen drag at
// zoom=2 should translate the element by 50 doc units, not 100.
// This catches a class of bugs where the SelectTool would translate
// by screen delta instead of doc delta.
TEST(RenderPaneClickTest, DragMovesElementByDocDeltaUnderZoom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoRectSvg));
  ViewportState v =
      MakeViewportFor(app, Vector2d(0.0, 0.0), Vector2d(800.0, 600.0));
  v.zoomAround(2.0, v.paneCenter());

  SelectTool tool;
  const Vector2d startScreen = v.documentToScreen(Vector2d(40.0, 40.0));
  tool.onMouseDown(app, v.screenToDocument(startScreen), MouseModifiers{});
  ASSERT_TRUE(app.hasSelection());

  auto rect = app.selectedElement()->cast<svg::SVGGraphicsElement>();
  const Transform2d startTransform = rect.transform();

  // Drag 100 screen pixels right at zoom=2 → 50 doc units right.
  const Vector2d targetScreen = startScreen + Vector2d(100.0, 0.0);
  tool.onMouseMove(app, v.screenToDocument(targetScreen), /*buttonHeld=*/true);
  app.flushFrame();

  const Transform2d after = rect.transform();
  EXPECT_NEAR(after.data[4] - startTransform.data[4], 50.0, 1e-6);
  EXPECT_NEAR(after.data[5] - startTransform.data[5], 0.0, 1e-6);
}

}  // namespace
}  // namespace donner::editor
