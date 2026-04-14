#include "donner/editor/ViewportGeometry.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(ViewportGeometryTest, ComputesCenteredLayoutWithPanOffset) {
  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(Vector2d(20.0, 30.0), Vector2d(400.0, 300.0),
                                   Vector2d(100.0, 80.0), Vector2d(15.0, -10.0),
                                   Box2d::FromXYWH(0.0, 0.0, 100.0, 80.0));

  EXPECT_DOUBLE_EQ(layout.imageOrigin.x, 185.0);
  EXPECT_DOUBLE_EQ(layout.imageOrigin.y, 130.0);
  EXPECT_DOUBLE_EQ(layout.imageSize.x, 100.0);
  EXPECT_DOUBLE_EQ(layout.imageSize.y, 80.0);
}

TEST(ViewportGeometryTest, ScreenToDocumentMapsViewBoxCorners) {
  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(Vector2d(50.0, 75.0), Vector2d(300.0, 200.0),
                                   Vector2d(200.0, 100.0), Vector2d(0.0, 0.0),
                                   Box2d::FromXYWH(10.0, 20.0, 400.0, 200.0));

  const auto topLeft = layout.screenToDocument(layout.imageOrigin);
  ASSERT_TRUE(topLeft.has_value());
  EXPECT_DOUBLE_EQ(topLeft->x, 10.0);
  EXPECT_DOUBLE_EQ(topLeft->y, 20.0);

  const auto bottomRight = layout.screenToDocument(
      layout.imageOrigin + Vector2d(layout.imageSize.x, layout.imageSize.y));
  ASSERT_TRUE(bottomRight.has_value());
  EXPECT_DOUBLE_EQ(bottomRight->x, 410.0);
  EXPECT_DOUBLE_EQ(bottomRight->y, 220.0);
}

TEST(ViewportGeometryTest, DocumentToScreenRoundTripsThroughViewBox) {
  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(Vector2d(0.0, 0.0), Vector2d(640.0, 480.0),
                                   Vector2d(320.0, 160.0), Vector2d(-12.0, 18.0),
                                   Box2d::FromXYWH(100.0, 50.0, 200.0, 100.0));

  const Vector2d documentPoint(150.0, 75.0);
  const auto screenPoint = layout.documentToScreen(documentPoint);
  ASSERT_TRUE(screenPoint.has_value());

  const auto mappedBack = layout.screenToDocument(*screenPoint);
  ASSERT_TRUE(mappedBack.has_value());
  EXPECT_DOUBLE_EQ(mappedBack->x, documentPoint.x);
  EXPECT_DOUBLE_EQ(mappedBack->y, documentPoint.y);
}

// Regression for the M4 click-offset bug. The scenario the editor hits:
// the render pane sits inside a larger window at pane-origin (560, 0),
// with a 720x800 content region. The rendered SVG is 720x800 (canvas
// matches pane), zoom is 1.0, pan is (0, 0). A click at screen position
// (920, 400) — the horizontal/vertical center of the image — should land
// at document coordinates (360, 400) when the SVG viewBox is (0 0 720 800).
TEST(ViewportGeometryTest, EditorClickCenterOfImageMapsToCenterOfDocument) {
  const Vector2d paneOrigin(560.0, 0.0);
  const Vector2d paneSize(720.0, 800.0);
  const Vector2d imageSize(720.0, 800.0);  // zoom=1, canvas=pane
  const Vector2d panOffset(0.0, 0.0);
  const Box2d docViewBox = Box2d::FromXYWH(0.0, 0.0, 720.0, 800.0);

  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(paneOrigin, paneSize, imageSize, panOffset, docViewBox);

  // The image should fill the pane exactly.
  EXPECT_DOUBLE_EQ(layout.imageOrigin.x, 560.0);
  EXPECT_DOUBLE_EQ(layout.imageOrigin.y, 0.0);

  // Click at pane center (screen 920, 400) → document center (360, 400).
  const auto center = layout.screenToDocument(Vector2d(920.0, 400.0));
  ASSERT_TRUE(center.has_value());
  EXPECT_DOUBLE_EQ(center->x, 360.0);
  EXPECT_DOUBLE_EQ(center->y, 400.0);

  // Top-left of the image should map to (0, 0).
  const auto topLeft = layout.screenToDocument(Vector2d(560.0, 0.0));
  ASSERT_TRUE(topLeft.has_value());
  EXPECT_DOUBLE_EQ(topLeft->x, 0.0);
  EXPECT_DOUBLE_EQ(topLeft->y, 0.0);

  // Bottom-right of the image should map to (720, 800).
  const auto bottomRight = layout.screenToDocument(Vector2d(1280.0, 800.0));
  ASSERT_TRUE(bottomRight.has_value());
  EXPECT_DOUBLE_EQ(bottomRight->x, 720.0);
  EXPECT_DOUBLE_EQ(bottomRight->y, 800.0);
}

// Same scenario but with zoom=2 and pan=(-100, -50). A click at the
// center of the *displayed* (zoomed) image should land at document
// (360, 400).
TEST(ViewportGeometryTest, EditorClickWithZoomAndPan) {
  const Vector2d paneOrigin(560.0, 0.0);
  const Vector2d paneSize(720.0, 800.0);
  // Texture is 720x800, zoom is 2 → displayed image is 1440x1600.
  const Vector2d imageSize(1440.0, 1600.0);
  const Vector2d panOffset(-100.0, -50.0);
  const Box2d docViewBox = Box2d::FromXYWH(0.0, 0.0, 720.0, 800.0);

  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(paneOrigin, paneSize, imageSize, panOffset, docViewBox);

  // With zoom=2 and pan=(-100, -50):
  //   imageOrigin.x = 560 + (720 - 1440) / 2 + (-100) = 560 + (-360) - 100 = 100
  //   imageOrigin.y = 0 + (800 - 1600) / 2 + (-50) = -400 - 50 = -450
  EXPECT_DOUBLE_EQ(layout.imageOrigin.x, 100.0);
  EXPECT_DOUBLE_EQ(layout.imageOrigin.y, -450.0);

  // A click at screen (100 + 720, -450 + 800) = (820, 350) corresponds to
  // the center of the displayed image. In document space that's (360, 400).
  const auto center = layout.screenToDocument(Vector2d(820.0, 350.0));
  ASSERT_TRUE(center.has_value());
  EXPECT_DOUBLE_EQ(center->x, 360.0);
  EXPECT_DOUBLE_EQ(center->y, 400.0);
}

// Viewbox-independent: click math should not care about which document-
// space range the image represents as long as `documentViewBox` is set
// correctly. The editor uses the document's canvas size to build the
// viewBox, so this test uses a 200x200 viewBox with a 720x800 displayed
// image (i.e. the document has been scaled to fit a larger pane).
TEST(ViewportGeometryTest, EditorClickWithScaledDocument) {
  const Vector2d paneOrigin(560.0, 0.0);
  const Vector2d paneSize(720.0, 800.0);
  const Vector2d imageSize(720.0, 800.0);  // zoom=1
  const Vector2d panOffset(0.0, 0.0);
  // Document has viewBox "0 0 200 200" — smaller than the rendered pane.
  const Box2d docViewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 200.0);

  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(paneOrigin, paneSize, imageSize, panOffset, docViewBox);

  // Click at pane center (screen 920, 400) → document center (100, 100).
  const auto center = layout.screenToDocument(Vector2d(920.0, 400.0));
  ASSERT_TRUE(center.has_value());
  EXPECT_DOUBLE_EQ(center->x, 100.0);
  EXPECT_DOUBLE_EQ(center->y, 100.0);

  // Top-left → document (0, 0).
  const auto topLeft = layout.screenToDocument(Vector2d(560.0, 0.0));
  ASSERT_TRUE(topLeft.has_value());
  EXPECT_DOUBLE_EQ(topLeft->x, 0.0);
  EXPECT_DOUBLE_EQ(topLeft->y, 0.0);

  // Bottom-right → document (200, 200).
  const auto bottomRight = layout.screenToDocument(Vector2d(1280.0, 800.0));
  ASSERT_TRUE(bottomRight.has_value());
  EXPECT_DOUBLE_EQ(bottomRight->x, 200.0);
  EXPECT_DOUBLE_EQ(bottomRight->y, 200.0);
}

TEST(ViewportGeometryTest, RejectsCoordinateConversionWhenImageIsMissing) {
  const DrawingViewportLayout layout =
      ComputeDrawingViewportLayout(Vector2d(0.0, 0.0), Vector2d(100.0, 100.0),
                                   Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                   Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0));

  EXPECT_FALSE(layout.hasImage());
  EXPECT_FALSE(layout.containsScreenPoint(Vector2d(10.0, 10.0)));
  EXPECT_FALSE(layout.screenToDocument(Vector2d(10.0, 10.0)).has_value());
  EXPECT_FALSE(layout.documentToScreen(Vector2d(10.0, 10.0)).has_value());
}

}  // namespace
}  // namespace donner::editor
