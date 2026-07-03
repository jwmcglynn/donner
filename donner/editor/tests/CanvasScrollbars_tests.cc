#include "donner/editor/CanvasScrollbars.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

using testing::DoubleNear;

/// A 100x100 document in a 400x300 pane at (50, 20).
ViewportState MakeViewport(double zoom) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(50.0, 20.0);
  viewport.paneSize = Vector2d(400.0, 300.0);
  viewport.devicePixelRatio = 2.0;
  viewport.documentViewBox = Box2d(Vector2d::Zero(), Vector2d(100.0, 100.0));
  viewport.zoom = zoom;
  // Anchor the document center to the pane center.
  viewport.panDocPoint = Vector2d(50.0, 50.0);
  viewport.panScreenPoint = Vector2d(250.0, 170.0);
  return viewport;
}

TEST(CanvasScrollbars, HiddenWhenDocumentFitsInPane) {
  const CanvasScrollbars bars = ComputeCanvasScrollbars(MakeViewport(1.0));
  EXPECT_FALSE(bars.horizontal.visible);
  EXPECT_FALSE(bars.vertical.visible);
}

TEST(CanvasScrollbars, VisibleWithPaneFractionThumbWhenZoomedIn) {
  // Zoom 8: the document spans 800x800 screen px inside a 400x300 pane.
  const ViewportState viewport = MakeViewport(8.0);
  const CanvasScrollbars bars = ComputeCanvasScrollbars(viewport);

  ASSERT_TRUE(bars.horizontal.visible);
  ASSERT_TRUE(bars.vertical.visible);
  EXPECT_THAT(bars.horizontal.railStart, DoubleNear(50.0, 1e-9));
  EXPECT_THAT(bars.horizontal.railLength, DoubleNear(400.0, 1e-9));

  // Content extent = document (800) since it fully contains the pane;
  // thumb = pane fraction of the rail.
  EXPECT_THAT(bars.horizontal.thumbLength, DoubleNear(400.0 * 400.0 / 800.0, 1e-9));
  EXPECT_THAT(bars.vertical.thumbLength, DoubleNear(300.0 * 300.0 / 800.0, 1e-6));

  // Centered viewport: the horizontal thumb sits centered on its rail.
  const double thumbTravel = 400.0 - bars.horizontal.thumbLength;
  EXPECT_THAT(bars.horizontal.thumbStart, DoubleNear(50.0 + thumbTravel / 2.0, 1e-9));

  // Dragging the thumb across its full travel scrolls across the full
  // off-pane content.
  EXPECT_THAT(bars.horizontal.contentPerThumbPx * thumbTravel, DoubleNear(800.0 - 400.0, 1e-9));
}

TEST(CanvasScrollbars, ThumbTracksPan) {
  ViewportState viewport = MakeViewport(8.0);
  const double centeredThumbStart = ComputeCanvasScrollbars(viewport).horizontal.thumbStart;

  // Pan the content right (viewport moves toward the document's left edge):
  // the thumb moves left.
  viewport.panBy(Vector2d(100.0, 0.0));
  const CanvasScrollbarAxis panned = ComputeCanvasScrollbars(viewport).horizontal;
  ASSERT_TRUE(panned.visible);
  EXPECT_LT(panned.thumbStart, centeredThumbStart);
}

TEST(CanvasScrollbars, ContentExtentIncludesPaneWhenPannedPastDocument) {
  ViewportState viewport = MakeViewport(8.0);
  // Pan far left so the document sits entirely left of the pane.
  viewport.panBy(Vector2d(-2000.0, 0.0));
  const CanvasScrollbarAxis axis = ComputeCanvasScrollbars(viewport).horizontal;
  ASSERT_TRUE(axis.visible);
  // The thumb parks at the far right end of the rail (the pane is the
  // rightmost part of the content extent) and stays grabbable.
  EXPECT_GE(axis.thumbLength, kCanvasScrollbarMinThumbPx);
  EXPECT_THAT(axis.thumbStart + axis.thumbLength, DoubleNear(50.0 + 400.0, 1e-6));
}

}  // namespace
}  // namespace donner::editor
