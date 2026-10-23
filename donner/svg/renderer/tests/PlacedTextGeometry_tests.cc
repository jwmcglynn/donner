#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

#include "donner/base/Path.h"
#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {
namespace {

using testing::ElementsAre;

StretchedTextPath MeasurePath(const Path& path) {
  return StretchedTextPath{path.measure(), 0.0, false};
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
}

}  // namespace
}  // namespace donner::svg
