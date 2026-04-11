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
