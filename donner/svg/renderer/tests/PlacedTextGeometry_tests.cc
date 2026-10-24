#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

#include "donner/base/Path.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/text/TextPathWarp.h"

namespace donner::svg {
namespace {

using testing::Contains;
using testing::DoubleNear;
using testing::ElementsAre;

StretchedTextPath MeasurePath(const Path& path) {
  Path::MeasuredPath measured = path.measure();
  std::vector<double> ends = measured.segmentEndDistances();
  const auto commands = path.commands();
  const bool linear = commands.size() == 2u && commands[0].verb == Path::Verb::MoveTo &&
                      commands[1].verb == Path::Verb::LineTo;
  return StretchedTextPath{std::move(measured), std::move(ends), 0.0, false, linear};
}

TEST(PlacedTextGeometryTest, StretchWarpsCurvedOutlineControlPointsAcrossPathTurn) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(4.0, 0.0), Vector2d(10.0, 6.0), Vector2d(10.0, 10.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .curveTo(Vector2d(5.0, -2.0), Vector2d(12.0, -2.0), Vector2d(15.0, -2.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph, 1.0);

  ASSERT_FALSE(warped.empty());
  EXPECT_GE(warped.points().size(), 3u);
  EXPECT_GT(warped.bounds().bottomRight.y - warped.bounds().topLeft.y, 5.0);
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

TEST(PlacedTextGeometryTest, StretchJoinsOneSidedNormalsAcrossSharpCorner) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path forward =
      PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  const Path reverse = PathBuilder().moveTo(Vector2d(10.0, 2.0)).lineTo(Vector2d(0.0, 2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;
  const StretchedTextPath mapping = MeasurePath(route);

  const Path warpedForward = WarpGlyphOutline(forward, mapping, glyph, 0.05);
  const Path warpedReverse = WarpGlyphOutline(reverse, mapping, glyph, 0.05);

  ASSERT_FALSE(warpedForward.empty());
  ASSERT_FALSE(warpedReverse.empty());
  EXPECT_THAT(warpedForward.points(), Contains(Vector2Near(10.0, -2.0)));
  EXPECT_THAT(warpedForward.points(), Contains(Vector2Near(12.0, 0.0)));
  EXPECT_THAT(warpedReverse.points(), Contains(Vector2Near(8.0, 0.0)));
  EXPECT_THAT(warpedReverse.points(), Contains(Vector2Near(10.0, 2.0)));
}

TEST(PlacedTextGeometryTest, StretchQuadraticCrossesSharpCornerWithNormalOffset) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .quadTo(Vector2d(4.0, -3.0), Vector2d(10.0, -2.0))
                           .build();
  const Path reverse = PathBuilder()
                           .moveTo(Vector2d(10.0, -2.0))
                           .quadTo(Vector2d(6.0, -3.0), Vector2d(0.0, -2.0))
                           .build();
  const Path startsAtCorner = PathBuilder()
                                  .moveTo(Vector2d(5.0, -2.0))
                                  .quadTo(Vector2d(7.0, -3.0), Vector2d(10.0, -2.0))
                                  .build();
  const Path endsAtCorner = PathBuilder()
                                .moveTo(Vector2d(0.0, -2.0))
                                .quadTo(Vector2d(2.0, -3.0), Vector2d(5.0, -2.0))
                                .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05);

  EXPECT_FALSE(warped.empty());
  EXPECT_FALSE(WarpGlyphOutline(reverse, MeasurePath(route), glyph, 0.05).empty());
  EXPECT_FALSE(WarpGlyphOutline(startsAtCorner, MeasurePath(route), glyph, 0.05).empty());
  EXPECT_FALSE(WarpGlyphOutline(endsAtCorner, MeasurePath(route), glyph, 0.05).empty());
}

TEST(PlacedTextGeometryTest, StretchCubicCrossesSharpCornerWithNormalOffset) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .curveTo(Vector2d(3.0, -3.0), Vector2d(7.0, -3.0),
                                    Vector2d(10.0, -2.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05);

  EXPECT_FALSE(warped.empty());

  const Path closedRoute = PathBuilder()
                               .moveTo(Vector2d(0.0, 0.0))
                               .lineTo(Vector2d(10.0, 0.0))
                               .lineTo(Vector2d(10.0, 10.0))
                               .lineTo(Vector2d(0.0, 10.0))
                               .closePath()
                               .build();
  StretchedTextPath closedMapping = MeasurePath(closedRoute);
  closedMapping.closed = true;
  glyph.textPathOrigin = closedMapping.measuredPath.pathLength() - 5.0;
  EXPECT_FALSE(WarpGlyphOutline(outline, closedMapping, glyph, 0.05).empty());
}

TEST(PlacedTextGeometryTest, StretchUsesOneSidedNormalsAtSharpCornerEndpoints) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path startsAtCorner =
      PathBuilder().moveTo(Vector2d(5.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  const Path endsAtCorner =
      PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(5.0, -2.0)).build();
  const Path consecutive = PathBuilder()
                               .moveTo(Vector2d(0.0, -2.0))
                               .lineTo(Vector2d(5.0, -2.0))
                               .lineTo(Vector2d(10.0, -2.0))
                               .build();
  const Path constantAtCorner =
      PathBuilder().moveTo(Vector2d(5.0, -2.0)).lineTo(Vector2d(5.0, 2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;
  const StretchedTextPath mapping = MeasurePath(route);

  const Path warpedStart = WarpGlyphOutline(startsAtCorner, mapping, glyph, 0.05);
  const Path warpedEnd = WarpGlyphOutline(endsAtCorner, mapping, glyph, 0.05);
  const Path warpedConsecutive = WarpGlyphOutline(consecutive, mapping, glyph, 0.05);
  const Path warpedConstant = WarpGlyphOutline(constantAtCorner, mapping, glyph, 0.05);

  ASSERT_FALSE(warpedStart.empty());
  ASSERT_FALSE(warpedEnd.empty());
  ASSERT_FALSE(warpedConsecutive.empty());
  ASSERT_FALSE(warpedConstant.empty());
  EXPECT_THAT(warpedStart.points(), Contains(Vector2Near(12.0, 0.0)));
  EXPECT_THAT(warpedEnd.points(), Contains(Vector2Near(10.0, -2.0)));
  EXPECT_THAT(warpedConsecutive.points(), Contains(Vector2Near(10.0, -2.0)));
  EXPECT_THAT(warpedConsecutive.points(), Contains(Vector2Near(12.0, 0.0)));
  EXPECT_THAT(warpedConstant.points(), Contains(Vector2Near(10.0, -2.0)));
  EXPECT_THAT(warpedConstant.points(), Contains(Vector2Near(10.0, 2.0)));
}

TEST(PlacedTextGeometryTest, StretchJoinsOneSidedNormalsAcrossClosedSeam) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .lineTo(Vector2d(0.0, 10.0))
                         .closePath()
                         .build();
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = mapping.measuredPath.pathLength() - 5.0;

  const Path warped = WarpGlyphOutline(edge, mapping, glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  EXPECT_THAT(warped.points(), Contains(Vector2Near(-2.0, 0.0)));
  EXPECT_THAT(warped.points(), Contains(Vector2Near(0.0, -2.0)));

  const Path startsAtSeam =
      PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(5.0, -2.0)).build();
  glyph.textPathOrigin = mapping.measuredPath.pathLength();
  const Path warpedStart = WarpGlyphOutline(startsAtSeam, mapping, glyph, 0.05);
  ASSERT_FALSE(warpedStart.empty());
  EXPECT_THAT(warpedStart.points(), Contains(Vector2Near(0.0, -2.0)));

  const Path constantAtSeam =
      PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(0.0, 2.0)).build();
  const Path warpedConstant = WarpGlyphOutline(constantAtSeam, mapping, glyph, 0.05);
  ASSERT_FALSE(warpedConstant.empty());
  EXPECT_THAT(warpedConstant.points(), Contains(Vector2Near(-2.0, 0.0)));
  EXPECT_THAT(warpedConstant.points(), Contains(Vector2Near(2.0, 0.0)));
}

TEST(PlacedTextGeometryTest, StretchDoesNotJoinCoincidentDisconnectedSubpaths) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .moveTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  EXPECT_TRUE(WarpGlyphOutline(edge, MeasurePath(route), glyph, 0.05).empty());
}

TEST(PlacedTextGeometryTest, StretchDoesNotWrapOpenGeometricReturnAsClosedSeam) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(0.0, 0.0))
                         .build();
  const StretchedTextPath mapping = MeasurePath(route);
  const double length = mapping.measuredPath.pathLength();

  const auto incoming =
      mapping.measuredPath.pointAtArcLength(length, Path::MeasuredPath::BoundarySide::Incoming);
  const auto outgoing =
      mapping.measuredPath.pointAtArcLength(length, Path::MeasuredPath::BoundarySide::Outgoing);

  ASSERT_TRUE(incoming.valid);
  ASSERT_TRUE(outgoing.valid);
  EXPECT_THAT(outgoing.angle, DoubleNear(incoming.angle, 1e-9));
}

TEST(PlacedTextGeometryTest, StretchSkipsBoundedConnectedZeroLengthSegmentsAtCorner) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path edge = PathBuilder().moveTo(Vector2d(5.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  const Path warped = WarpGlyphOutline(edge, MeasurePath(route), glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  EXPECT_THAT(warped.points(), Contains(Vector2Near(12.0, 0.0)));
}

TEST(PlacedTextGeometryTest, StretchRefusesMoreThanBoundedZeroLengthCornerSegments) {
  PathBuilder routeBuilder;
  routeBuilder.moveTo(Vector2d(0.0, 0.0)).lineTo(Vector2d(10.0, 0.0));
  for (int i = 0; i < 65; ++i) {
    routeBuilder.lineTo(Vector2d(10.0, 0.0));
  }
  routeBuilder.lineTo(Vector2d(10.0, 10.0));
  const Path edge = PathBuilder().moveTo(Vector2d(5.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  EXPECT_THAT(WarpGlyphOutline(edge, MeasurePath(routeBuilder.build()), glyph, 0.05),
              testing::Property(&Path::empty, true));
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
  const StretchedTextPath mapping = MeasurePath(route);
  const double length = mapping.measuredPath.pathLength();
  const Path edge =
      PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(length, -2.0)).build();
  const Path reverseEdge =
      PathBuilder().moveTo(Vector2d(length, -2.0)).lineTo(Vector2d(0.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(edge, mapping, glyph, 0.05);
  const Path reverse = WarpGlyphOutline(reverseEdge, mapping, glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  ASSERT_FALSE(reverse.empty());
  EXPECT_GT(warped.points().size(), 16u);
  EXPECT_GT(warped.bounds().bottomRight.y - warped.bounds().topLeft.y, 10.0);
  ASSERT_EQ(warped.points().size(), reverse.points().size());
  for (size_t i = 0; i < warped.points().size(); ++i) {
    const Vector2d expected = reverse.points()[reverse.points().size() - 1u - i];
    EXPECT_NEAR(warped.points()[i].x, expected.x, 0.05);
    EXPECT_NEAR(warped.points()[i].y, expected.y, 0.05);
  }
}

TEST(PlacedTextGeometryTest, StretchClosedPathSplitsTranslatedWrapBoundaries) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(10.0, 0.0))
                         .curveTo(Vector2d(15.5, 0.0), Vector2d(20.0, 4.5), Vector2d(20.0, 10.0))
                         .curveTo(Vector2d(20.0, 15.5), Vector2d(15.5, 20.0), Vector2d(10.0, 20.0))
                         .curveTo(Vector2d(4.5, 20.0), Vector2d(0.0, 15.5), Vector2d(0.0, 10.0))
                         .curveTo(Vector2d(0.0, 4.5), Vector2d(4.5, 0.0), Vector2d(10.0, 0.0))
                         .closePath()
                         .build();
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const double length = mapping.measuredPath.pathLength();
  const Path edge =
      PathBuilder().moveTo(Vector2d(0.0, -1.0)).lineTo(Vector2d(length * 0.5, -1.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = length * 0.75;

  const Path warped = WarpGlyphOutline(edge, mapping, glyph, 1.0);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 4u);
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

TEST(PlacedTextGeometryTest, StretchCurvedOutlineUsesMappedErrorNotLocalFlatness) {
  const Path route =
      PathBuilder()
          .moveTo(Vector2d(1.0, 0.0))
          .curveTo(Vector2d(1.0, 0.5523), Vector2d(0.5523, 1.0), Vector2d(0.0, 1.0))
          .curveTo(Vector2d(-0.5523, 1.0), Vector2d(-1.0, 0.5523), Vector2d(-1.0, 0.0))
          .curveTo(Vector2d(-1.0, -0.5523), Vector2d(-0.5523, -1.0), Vector2d(0.0, -1.0))
          .curveTo(Vector2d(0.5523, -1.0), Vector2d(1.0, -0.5523), Vector2d(1.0, 0.0))
          .closePath()
          .build();
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, 100.0))
                           .quadTo(Vector2d(0.05, 105.0), Vector2d(0.0, 110.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(outline, mapping, glyph, 0.1);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 2u);
  EXPECT_GT(warped.bounds().bottomRight.x - warped.bounds().topLeft.x, 1.0);
}

TEST(PlacedTextGeometryTest, StretchCurveTurnbackCannotAliasClosedPathCircuits) {
  const Path route =
      PathBuilder()
          .moveTo(Vector2d(1.0, 0.0))
          .curveTo(Vector2d(1.0, 0.5523), Vector2d(0.5523, 1.0), Vector2d(0.0, 1.0))
          .curveTo(Vector2d(-0.5523, 1.0), Vector2d(-1.0, 0.5523), Vector2d(-1.0, 0.0))
          .curveTo(Vector2d(-1.0, -0.5523), Vector2d(-0.5523, -1.0), Vector2d(0.0, -1.0))
          .curveTo(Vector2d(0.5523, -1.0), Vector2d(1.0, -0.5523), Vector2d(1.0, 0.0))
          .closePath()
          .build();
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const double length = mapping.measuredPath.pathLength();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, 0.0))
                           .quadTo(Vector2d(length * 8.0, 0.0), Vector2d(0.0, 0.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const Path warped = WarpGlyphOutline(outline, mapping, glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 16u);
  EXPECT_GT(warped.bounds().bottomRight.x - warped.bounds().topLeft.x, 1.5);
  EXPECT_GT(warped.bounds().bottomRight.y - warped.bounds().topLeft.y, 1.5);
}

TEST(PlacedTextGeometryTest, StretchCurveAcceptanceMatchesEmittedLocalChord) {
  const Path route =
      PathBuilder()
          .moveTo(Vector2d(1.0, 0.0))
          .curveTo(Vector2d(1.0, 0.5523), Vector2d(0.5523, 1.0), Vector2d(0.0, 1.0))
          .curveTo(Vector2d(-0.5523, 1.0), Vector2d(-1.0, 0.5523), Vector2d(-1.0, 0.0))
          .curveTo(Vector2d(-1.0, -0.5523), Vector2d(-0.5523, -1.0), Vector2d(0.0, -1.0))
          .curveTo(Vector2d(0.5523, -1.0), Vector2d(1.0, -0.5523), Vector2d(1.0, 0.0))
          .closePath()
          .build();
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const double normal = 1.0 - 1.0 / std::cos(0.2);
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, normal))
                           .quadTo(Vector2d(0.2, -normal), Vector2d(0.4, normal))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = mapping.measuredPath.pathLength() - 0.2;

  const Path warped = WarpGlyphOutline(outline, mapping, glyph, 0.002);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 2u);
}

TEST(PlacedTextGeometryTest, StretchSplitsQuadraticAtNonDyadicXExtremum) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(0.0, 20.0), Vector2d(20.0, 20.0), Vector2d(20.0, 0.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .quadTo(Vector2d(1.0, -2.0), Vector2d(0.5, -2.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 4.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05);

  ASSERT_FALSE(warped.empty());
  EXPECT_GT(warped.points().size(), 2u);
}

TEST(PlacedTextGeometryTest, StretchSplitsUniformlyTinyQuadraticXExtremum) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(0.0, 20.0), Vector2d(20.0, 20.0), Vector2d(20.0, 0.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .quadTo(Vector2d(1e-20, -2.0), Vector2d(0.5e-20, -2.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 4.0;

  EXPECT_FALSE(WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05).empty());
}

TEST(PlacedTextGeometryTest, StretchRejectsOverflowingExtremaNormalization) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .curveTo(Vector2d(0.0, 10.0), Vector2d(20.0, 10.0), Vector2d(20.0, 0.0))
                         .build();
  const double maximum = std::numeric_limits<double>::max();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(-maximum, 0.0))
                           .quadTo(Vector2d(maximum, 0.0), Vector2d(0.0, 0.0))
                           .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 4.0;

  EXPECT_TRUE(WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05).empty());
}

}  // namespace
}  // namespace donner::svg
