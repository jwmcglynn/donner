#include "donner/editor/ViewportState.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>

namespace donner::editor {
namespace {

// `EXPECT_NEAR` for `Vector2d`. We don't have an existing
// gmock matcher for this in the editor test suite, so define one
// locally — the tests below use it everywhere a coordinate-mapping
// round-trip is expected to land within rounding tolerance.
::testing::AssertionResult NearVec(const Vector2d& actual, const Vector2d& expected,
                                   double tolerance) {
  const double dx = std::abs(actual.x - expected.x);
  const double dy = std::abs(actual.y - expected.y);
  if (dx > tolerance || dy > tolerance) {
    return ::testing::AssertionFailure()
           << "actual=(" << actual.x << ", " << actual.y << ") expected=(" << expected.x
           << ", " << expected.y << ") |dx|=" << dx << " |dy|=" << dy
           << " tolerance=" << tolerance;
  }
  return ::testing::AssertionSuccess();
}

#define EXPECT_NEAR_VEC(actual, expected, tolerance) \
  EXPECT_TRUE(NearVec((actual), (expected), (tolerance)))

/// Build a fresh ViewportState whose pane and viewBox match the args
/// and whose `resetTo100Percent` has been applied. Centralized so the
/// individual tests don't repeat the boilerplate.
ViewportState MakeFreshState(Vector2d paneOrigin, Vector2d paneSize, Box2d viewBox,
                             double dpr = 1.0) {
  ViewportState v;
  v.paneOrigin = paneOrigin;
  v.paneSize = paneSize;
  v.documentViewBox = viewBox;
  v.devicePixelRatio = dpr;
  v.resetTo100Percent();
  return v;
}

// ---------------------------------------------------------------------------
// Default state and reset
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, DefaultStateIsIdentityAt100Percent) {
  ViewportState v;
  EXPECT_DOUBLE_EQ(v.zoom, 1.0);
  EXPECT_DOUBLE_EQ(v.devicePixelRatio, 1.0);
  EXPECT_DOUBLE_EQ(v.pixelsPerDocUnit(), 1.0);
  EXPECT_DOUBLE_EQ(v.devicePixelsPerDocUnit(), 1.0);
}

TEST(ViewportStateTest, ResetTo100PercentAnchorsViewBoxCenterOnPaneCenter) {
  ViewportState v = MakeFreshState(/*paneOrigin=*/Vector2d(50.0, 75.0),
                                   /*paneSize=*/Vector2d(800.0, 600.0),
                                   /*viewBox=*/Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));
  EXPECT_DOUBLE_EQ(v.zoom, 1.0);
  // The center of the viewBox is at (100, 50), which after reset
  // should land on the pane center at (50+400, 75+300) = (450, 375).
  EXPECT_NEAR_VEC(v.documentToScreen(Vector2d(100.0, 50.0)), Vector2d(450.0, 375.0), 1e-9);
}

TEST(ViewportStateTest, ResetTo100PercentMakesOneDocUnitOneScreenPixel) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1000.0, 1000.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  // One step in document space = one pixel in screen space.
  const Vector2d origin = v.documentToScreen(Vector2d(0.0, 0.0));
  const Vector2d oneStepX = v.documentToScreen(Vector2d(1.0, 0.0));
  const Vector2d oneStepY = v.documentToScreen(Vector2d(0.0, 1.0));
  EXPECT_DOUBLE_EQ(oneStepX.x - origin.x, 1.0);
  EXPECT_DOUBLE_EQ(oneStepX.y - origin.y, 0.0);
  EXPECT_DOUBLE_EQ(oneStepY.x - origin.x, 0.0);
  EXPECT_DOUBLE_EQ(oneStepY.y - origin.y, 1.0);
}

// ---------------------------------------------------------------------------
// Round-trip invariants — the core property the design doc commits to.
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, ScreenToDocumentRoundTripsAt100Percent) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  for (auto p : {Vector2d(0.0, 0.0), Vector2d(50.0, 70.0), Vector2d(199.0, 199.0),
                 Vector2d(100.5, 50.25)}) {
    EXPECT_NEAR_VEC(v.screenToDocument(v.documentToScreen(p)), p, 1e-9);
  }
}

TEST(ViewportStateTest, ScreenToDocumentRoundTripsUnderZoomAndPan) {
  // Sweep over a few representative zoom/focal/pan combinations and
  // assert that round-tripping any document point through screen and
  // back lands within `1e-6` of the original.
  for (double zoom : {0.25, 0.5, 1.0, 2.0, 4.0, 16.0}) {
    for (Vector2d focal :
         {Vector2d(120.0, 80.0), Vector2d(0.0, 0.0), Vector2d(800.0, 600.0)}) {
      ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                       Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));
      v.zoomAround(zoom, focal);
      v.panBy(Vector2d(37.5, -12.0));
      for (auto docPoint :
           {Vector2d(0.0, 0.0), Vector2d(446.0, 256.0), Vector2d(892.0, 512.0),
            Vector2d(123.4, 56.7)}) {
        EXPECT_NEAR_VEC(v.screenToDocument(v.documentToScreen(docPoint)), docPoint, 1e-6)
            << "  zoom=" << zoom << " focal=(" << focal.x << "," << focal.y << ") doc=("
            << docPoint.x << "," << docPoint.y << ")";
      }
    }
  }
}

TEST(ViewportStateTest, DocumentToScreenRoundTripsTheOtherWay) {
  ViewportState v = MakeFreshState(Vector2d(560.0, 0.0), Vector2d(720.0, 800.0),
                                   Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));
  v.zoomAround(2.5, Vector2d(900.0, 320.0));
  v.panBy(Vector2d(-25.0, 17.0));
  for (auto screenPoint : {Vector2d(560.0, 0.0), Vector2d(900.0, 320.0), Vector2d(1280.0, 800.0),
                           Vector2d(723.5, 412.25)}) {
    EXPECT_NEAR_VEC(v.documentToScreen(v.screenToDocument(screenPoint)), screenPoint, 1e-6);
  }
}

// ---------------------------------------------------------------------------
// Zoom focal-point preservation — the bug from the user report.
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, ZoomAroundCursorPreservesFocalPoint) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));

  // Pick any screen point inside the rendered image.
  const Vector2d focal(900.0, 320.0);
  const Vector2d docBefore = v.screenToDocument(focal);
  v.zoomAround(2.5, focal);

  // Document point that *was* under `focal` is still under `focal`.
  EXPECT_NEAR_VEC(v.documentToScreen(docBefore), focal, 1e-9);
  EXPECT_DOUBLE_EQ(v.zoom, 2.5);
}

TEST(ViewportStateTest, ZoomAroundCursorPreservesFocalAcrossSweep) {
  // Same property but across a fan of focal points and zoom factors.
  // This is the test that catches the kind of "works at one zoom,
  // drifts at another" bug we saw in the old applyZoom.
  ViewportState base = MakeFreshState(Vector2d::Zero(), Vector2d(1000.0, 800.0),
                                      Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  for (double zoom : {0.3, 1.0, 1.5, 4.0, 12.0, 30.0}) {
    for (Vector2d focal : {Vector2d(100.0, 200.0), Vector2d(500.0, 400.0),
                           Vector2d(900.0, 700.0), Vector2d(0.0, 0.0)}) {
      ViewportState v = base;
      const Vector2d docBefore = v.screenToDocument(focal);
      v.zoomAround(zoom, focal);
      EXPECT_NEAR_VEC(v.documentToScreen(docBefore), focal, 1e-9)
          << "  zoom=" << zoom << " focal=(" << focal.x << "," << focal.y << ")";
    }
  }
}

TEST(ViewportStateTest, ZoomInThenOutReturnsToStart) {
  // 5 steps in, 5 steps out around the same focal point, all using
  // the same factor — should land exactly on the start zoom and the
  // start anchor screen position.
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 800.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  const Vector2d focal(420.0, 380.0);
  const Vector2d docBefore = v.screenToDocument(focal);
  for (int i = 0; i < 5; ++i) {
    v.zoomAround(v.zoom * 1.5, focal);
  }
  for (int i = 0; i < 5; ++i) {
    v.zoomAround(v.zoom / 1.5, focal);
  }
  EXPECT_NEAR(v.zoom, 1.0, 1e-9);
  EXPECT_NEAR_VEC(v.documentToScreen(docBefore), focal, 1e-9);
}

TEST(ViewportStateTest, ZoomClampedToMinAndMax) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                   Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));
  v.zoomAround(1000.0, Vector2d(400.0, 300.0));
  EXPECT_DOUBLE_EQ(v.zoom, ViewportState::kMaxZoom);
  v.zoomAround(0.0001, Vector2d(400.0, 300.0));
  EXPECT_DOUBLE_EQ(v.zoom, ViewportState::kMinZoom);
}

// ---------------------------------------------------------------------------
// Pan invariants
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, PanByMovesEveryPointByDelta) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));
  v.zoomAround(3.0, Vector2d(640.0, 360.0));

  const Vector2d delta(50.0, -25.0);
  // A few representative document points — pan must shift their
  // on-screen positions by exactly `delta`, regardless of where they
  // are in the document.
  for (auto docPoint :
       {Vector2d(0.0, 0.0), Vector2d(446.0, 256.0), Vector2d(892.0, 512.0)}) {
    const Vector2d screenBefore = v.documentToScreen(docPoint);
    ViewportState w = v;
    w.panBy(delta);
    EXPECT_NEAR_VEC(w.documentToScreen(docPoint), screenBefore + delta, 1e-9)
        << "  doc=(" << docPoint.x << "," << docPoint.y << ")";
  }
}

TEST(ViewportStateTest, PanByZeroIsNoOp) {
  ViewportState v = MakeFreshState(Vector2d(560.0, 0.0), Vector2d(720.0, 800.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  v.zoomAround(1.7, Vector2d(900.0, 350.0));
  const ViewportState before = v;
  v.panBy(Vector2d::Zero());
  EXPECT_EQ(v.zoom, before.zoom);
  EXPECT_EQ(v.panDocPoint, before.panDocPoint);
  EXPECT_EQ(v.panScreenPoint, before.panScreenPoint);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, ResetReturnsTo100PercentRegardlessOfHistory) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1000.0, 800.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0));
  v.zoomAround(7.0, Vector2d(300.0, 250.0));
  v.panBy(Vector2d(200.0, -150.0));
  v.zoomAround(0.4, Vector2d(50.0, 50.0));
  v.panBy(Vector2d(-300.0, 100.0));
  v.resetTo100Percent();
  EXPECT_DOUBLE_EQ(v.zoom, 1.0);
  EXPECT_NEAR_VEC(v.documentToScreen(Vector2d(100.0, 100.0)), Vector2d(500.0, 400.0), 1e-9);
}

// ---------------------------------------------------------------------------
// imageScreenRect
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, ImageScreenRectMatchesViewBoxAt100Percent) {
  // 200x100 viewBox, 800x600 pane → at 100% the on-screen image is
  // 200x100, centered, top-left at (300, 250).
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));
  const Box2d rect = v.imageScreenRect();
  EXPECT_NEAR_VEC(rect.topLeft, Vector2d(300.0, 250.0), 1e-9);
  EXPECT_NEAR_VEC(rect.bottomRight, Vector2d(500.0, 350.0), 1e-9);
}

TEST(ViewportStateTest, ImageScreenRectScalesWithZoom) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));
  v.zoomAround(2.0, Vector2d(400.0, 300.0));  // Around the pane center.
  const Box2d rect = v.imageScreenRect();
  // 200x100 doubled = 400x200, still centered at (400, 300).
  EXPECT_NEAR_VEC(rect.topLeft, Vector2d(200.0, 200.0), 1e-9);
  EXPECT_NEAR_VEC(rect.bottomRight, Vector2d(600.0, 400.0), 1e-9);
}

// ---------------------------------------------------------------------------
// desiredCanvasSize
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, DesiredCanvasSizeMatchesViewBoxAt100PercentDpr1) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));
  const Vector2i size = v.desiredCanvasSize();
  EXPECT_EQ(size.x, 892);
  EXPECT_EQ(size.y, 512);
}

TEST(ViewportStateTest, DesiredCanvasSizeScalesWithZoom) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0));
  v.zoomAround(3.0, Vector2d(640.0, 360.0));
  const Vector2i size = v.desiredCanvasSize();
  EXPECT_EQ(size.x, 600);
  EXPECT_EQ(size.y, 300);
}

TEST(ViewportStateTest, DesiredCanvasSizeScalesWithDpr) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0), /*dpr=*/2.0);
  // At 100% zoom, dpr=2 should give us a 2x-resolution bitmap so the
  // SVG rasterizer outputs full-fidelity pixels at native.
  const Vector2i size = v.desiredCanvasSize();
  EXPECT_EQ(size.x, 400);
  EXPECT_EQ(size.y, 200);
}

TEST(ViewportStateTest, DesiredCanvasSizeScalesWithDprAt2x) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                   Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0), /*dpr=*/2.0);
  // 200 viewBox * 1 zoom * 2 dpr = 400 device pixels per axis.
  const Vector2i size = v.desiredCanvasSize();
  EXPECT_EQ(size.x, 400);
  EXPECT_EQ(size.y, 400);
}

TEST(ViewportStateTest, ImageScreenRectIsDprInvariant) {
  // Critical screen-coordinate invariant: changing DPR must not move
  // the on-screen rectangle. DPR only affects how many device pixels
  // the rasterizer produces; the logical pixel coordinates a user
  // sees and clicks are unchanged.
  ViewportState v1 = MakeFreshState(Vector2d(50.0, 30.0), Vector2d(800.0, 600.0),
                                    Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0),
                                    /*dpr=*/1.0);
  v1.zoomAround(2.5, v1.paneCenter());
  v1.panBy(Vector2d(40.0, -25.0));

  ViewportState v2 = v1;
  v2.devicePixelRatio = 2.0;

  const Box2d rect1 = v1.imageScreenRect();
  const Box2d rect2 = v2.imageScreenRect();
  EXPECT_NEAR_VEC(rect1.topLeft, rect2.topLeft, 1e-9);
  EXPECT_NEAR_VEC(rect1.bottomRight, rect2.bottomRight, 1e-9);
}

TEST(ViewportStateTest, ScreenToDocumentIsDprInvariant) {
  // Same property as the screen-rect invariant but for click math:
  // the screen-pixel → document-point mapping must not depend on
  // DPR. ImGui mouse positions are in *logical* pixels, so the
  // viewport math must treat them the same way regardless of how
  // many physical pixels each logical pixel takes.
  ViewportState v1 = MakeFreshState(Vector2d::Zero(), Vector2d(800.0, 600.0),
                                    Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0),
                                    /*dpr=*/1.0);
  v1.zoomAround(3.0, v1.paneCenter());

  ViewportState v3 = v1;
  v3.devicePixelRatio = 3.0;

  for (auto screenPoint : {Vector2d(100.0, 100.0), Vector2d(400.0, 300.0),
                           Vector2d(750.5, 599.25)}) {
    EXPECT_NEAR_VEC(v1.screenToDocument(screenPoint), v3.screenToDocument(screenPoint),
                    1e-9);
  }
}

TEST(ViewportStateTest, DesiredCanvasSizeClampsToMaxDim) {
  ViewportState v = MakeFreshState(Vector2d::Zero(), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 1000.0, 1000.0));
  v.zoomAround(ViewportState::kMaxZoom, Vector2d(640.0, 360.0));
  const Vector2i size = v.desiredCanvasSize();
  // 1000 × 32 = 32000, well over the 8192 cap.
  EXPECT_EQ(size.x, ViewportState::kMaxCanvasDim);
  EXPECT_EQ(size.y, ViewportState::kMaxCanvasDim);
}

// ---------------------------------------------------------------------------
// Box transforms
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, BoxTransformsAreInverses) {
  ViewportState v = MakeFreshState(Vector2d(50.0, 30.0), Vector2d(1280.0, 720.0),
                                   Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0));
  v.zoomAround(2.5, Vector2d(640.0, 360.0));
  v.panBy(Vector2d(40.0, -15.0));

  const Box2d docBox = Box2d::FromXYWH(100.0, 50.0, 200.0, 80.0);
  const Box2d screenBox = v.documentToScreen(docBox);
  const Box2d backToDoc = v.screenToDocument(screenBox);
  EXPECT_NEAR_VEC(backToDoc.topLeft, docBox.topLeft, 1e-6);
  EXPECT_NEAR_VEC(backToDoc.bottomRight, docBox.bottomRight, 1e-6);
}

// ---------------------------------------------------------------------------
// Degenerate inputs
// ---------------------------------------------------------------------------

TEST(ViewportStateTest, ScreenToDocumentDoesNotDivideByZero) {
  ViewportState v;
  v.zoom = 0.0;  // Force the degenerate path.
  // Should return panDocPoint instead of NaN/Inf.
  const Vector2d result = v.screenToDocument(Vector2d(100.0, 200.0));
  EXPECT_TRUE(std::isfinite(result.x));
  EXPECT_TRUE(std::isfinite(result.y));
}

TEST(ViewportStateTest, DesiredCanvasSizeHandlesZeroViewBox) {
  ViewportState v;
  v.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 0.0, 0.0);
  const Vector2i size = v.desiredCanvasSize();
  EXPECT_GE(size.x, 1);
  EXPECT_GE(size.y, 1);
}

}  // namespace
}  // namespace donner::editor
