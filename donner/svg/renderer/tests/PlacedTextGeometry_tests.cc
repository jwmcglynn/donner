#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

#include "donner/base/Path.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/text/TextPathWarp.h"

namespace donner::svg {
namespace {

using testing::ElementsAre;

StretchedTextPath MeasurePath(const Path& path) {
  Path::MeasuredPath measured = path.measure();
  std::vector<double> ends = measured.segmentEndDistances();
  return StretchedTextPath{std::move(measured), std::move(ends), 0.0, false};
}

TEST(PlacedTextGeometryTest, StretchWarpsCurvedOutlineControlPointsAcrossPathTurn) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .curveTo(Vector2d(5.0, -2.0), Vector2d(12.0, -2.0), Vector2d(15.0, -2.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph);

  EXPECT_THAT(warped.points(), ElementsAre(Vector2Near(0.0, -2.0), Vector2Near(5.0, -2.0),
                                           Vector2Near(12.0, 2.0), Vector2Near(12.0, 5.0)));
}

TEST(PlacedTextGeometryTest, StretchIsIdentityOnHorizontalPathIncludingTangentExtensions) {
  const Path route = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(10.0, 0.0)).build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(-2.0, -3.0))
                           .quadTo(Vector2d(5.0, 4.0), Vector2d(12.0, -1.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph);

  ASSERT_EQ(outline.commands().size(), 2u);
  EXPECT_THAT(warped.commands(), ElementsAre(outline.commands()[0], outline.commands()[1]));
  EXPECT_THAT(warped.points(),
              ElementsAre(Vector2Near(-2.0, -3.0), Vector2Near(5.0, 4.0), Vector2Near(12.0, -1.0)));
}

TEST(PlacedTextGeometryTest, StretchRejectsNonfiniteGlyphCoordinates) {
  const Path route = PathBuilder().moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(10.0, 0.0)).build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, 0.0))
                           .lineTo(Vector2d(std::numeric_limits<double>::infinity(), 0.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  EXPECT_TRUE(WarpGlyphOutline(outline, MeasurePath(route), glyph).empty());
}

TEST(PlacedTextGeometryTest, StretchSubdividesWideRectangularGlyphEdgesAcrossCurve) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(0.0, 20.0), Vector2d(20.0, 20.0), Vector2d(20.0, 0.0))
                         .build();
  const Path rectangle = PathBuilder().addRect(Box2d::FromXYWH(0.0, -2.0, 18.0, 4.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(rectangle, MeasurePath(route), glyph);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.verbCount(), rectangle.verbCount());
  EXPECT_GT(warped.points().size(), rectangle.points().size());
  const Box2d bounds = warped.bounds();
  EXPECT_GT(bounds.bottomRight.y - bounds.topLeft.y, 4.0);
  EXPECT_GT(bounds.bottomRight.x - bounds.topLeft.x, 10.0);

  const Path tighter = WarpTextPathOutline(rectangle, MeasurePath(route), glyph, 0.01);
  ASSERT_FALSE(tighter.empty());
  EXPECT_GT(tighter.points().size(), warped.points().size());
  EXPECT_TRUE(WarpTextPathOutline(rectangle, MeasurePath(route), glyph, 0.0).empty());
}

TEST(PlacedTextGeometryTest, StretchSplitsRepeatedWaveAtMeasuredSegmentBoundaries) {
  PathBuilder routeBuilder;
  routeBuilder.moveTo(Vector2d(0.0, 0.0));
  for (int period = 0; period < 4; ++period) {
    const double x = period * 20.0;
    routeBuilder.curveTo(Vector2d(x + 5.0, 20.0), Vector2d(x + 15.0, -20.0),
                         Vector2d(x + 20.0, 0.0));
  }
  const Path route = routeBuilder.build();
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(80.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(edge, MeasurePath(route), glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 16u);
  EXPECT_GT(warped.bounds().bottomRight.y - warped.bounds().topLeft.y, 10.0);
}

TEST(PlacedTextGeometryTest, StretchDeviceScaleTightensToleranceAndCurveApproximation) {
  const double ordinaryTolerance = TextWarpToleranceFor(Transform2d());
  const double zoomedTolerance = TextWarpToleranceFor(Transform2d::Scale(32.0));
  EXPECT_LT(zoomedTolerance, ordinaryTolerance);

  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(0.0, 40.0), Vector2d(40.0, -40.0), Vector2d(40.0, 0.0))
                         .build();
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -3.0)).lineTo(Vector2d(40.0, -3.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;
  const StretchedTextPath mapping = MeasurePath(route);

  const Path ordinary = WarpGlyphOutline(edge, mapping, glyph, ordinaryTolerance);
  const Path zoomed = WarpGlyphOutline(edge, mapping, glyph, zoomedTolerance);
  ASSERT_FALSE(ordinary.empty());
  ASSERT_FALSE(zoomed.empty());
  EXPECT_GT(zoomed.points().size(), ordinary.points().size());
}

}  // namespace
}  // namespace donner::svg
