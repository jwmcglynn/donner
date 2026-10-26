#include "donner/svg/renderer/PlacedTextGeometry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

#include "donner/base/MathUtils.h"
#include "donner/base/Path.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/text/TextPathWarp.h"

namespace donner::svg {
namespace {

using testing::Contains;
using testing::DoubleNear;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;

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

TEST(PlacedTextGeometryTest, StretchUsesLimitingTangentsAtCoincidentCurveEndpoints) {
  const std::array<Path, 6> routes = {
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .curveTo(Vector2d(0.0, 0.0), Vector2d(0.0, 10.0), Vector2d(0.0, 20.0))
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .curveTo(Vector2d(0.0, 0.0), Vector2d(0.0, 0.0), Vector2d(0.0, 20.0))
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .quadTo(Vector2d(0.0, 0.0), Vector2d(0.0, 20.0))
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .curveTo(Vector2d(0.0, 10.0), Vector2d(0.0, 20.0), Vector2d(0.0, 20.0))
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .curveTo(Vector2d(0.0, 20.0), Vector2d(0.0, 20.0), Vector2d(0.0, 20.0))
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .quadTo(Vector2d(0.0, 20.0), Vector2d(0.0, 20.0))
          .build(),
  };
  const Path rectangle = PathBuilder().addRect(Box2d::FromXYWH(0.0, -2.0, 10.0, 4.0)).build();

  for (size_t index = 0; index < routes.size(); ++index) {
    SCOPED_TRACE(index);
    StretchedTextPath mapping = MeasurePath(routes[index]);
    TextGlyph glyph;
    const bool samplesEnd = index >= 3u;
    glyph.textPathOrigin = samplesEnd ? mapping.measuredPath.pathLength() - 10.0 : 0.0;
    glyph.xAdvance = 10.0;
    TextRun run;
    run.stretchedTextPath = std::make_shared<StretchedTextPath>(mapping);

    const Vector2d endpointLocal(samplesEnd ? 10.0 : 0.0, -2.0);
    const auto mappedEndpoint = MapTextPathPoint(mapping, glyph, endpointLocal);
    EXPECT_THAT(mappedEndpoint, Optional(Vector2Near(2.0, samplesEnd ? 20.0 : 0.0)));
    EXPECT_THAT(WarpGlyphOutline(rectangle, mapping, glyph).commands(), Not(IsEmpty()));
    TextGlyph cellGlyph = glyph;
    cellGlyph.textPathOrigin = 0.0;
    cellGlyph.xAdvance = mapping.measuredPath.pathLength();
    const Path cell = PlaceStretchedTextPathCell(run, cellGlyph, 2.0, 2.0);
    EXPECT_THAT(cell.commands(), Not(IsEmpty()));
    EXPECT_THAT(cell.bounds(), BoxEq(Vector2d(-2.0, 0.0), Vector2d(2.0, 20.0)));
  }
}

TEST(PlacedTextGeometryTest, StretchDistinguishesTinyAndZeroCurveEndpointDerivatives) {
  const auto expectEndpoint = [](const Path& route, bool samplesEnd, double expectedAngle,
                                 const Vector2d& expectedMapped) {
    StretchedTextPath mapping = MeasurePath(route);
    const double distance = samplesEnd ? mapping.measuredPath.pathLength() : 0.0;
    const auto sample = mapping.measuredPath.pointAtArcLength(distance);
    ASSERT_TRUE(sample.valid);
    EXPECT_THAT(sample.angle, DoubleNear(expectedAngle, 1e-12));

    TextGlyph glyph;
    glyph.textPathOrigin = distance;
    EXPECT_THAT(MapTextPathPoint(mapping, glyph, Vector2d(0.0, -2.0)),
                Optional(Vector2Near(expectedMapped.x, expectedMapped.y)));
  };

  expectEndpoint(PathBuilder()
                     .moveTo(Vector2d(0.0, 0.0))
                     .curveTo(Vector2d(1e-20, 0.0), Vector2d(0.0, 10.0), Vector2d(0.0, 20.0))
                     .build(),
                 false, 0.0, Vector2d(0.0, -2.0));
  expectEndpoint(PathBuilder()
                     .moveTo(Vector2d(0.0, 0.0))
                     .curveTo(Vector2d(0.0, 10.0), Vector2d(1e-20, 20.0), Vector2d(0.0, 20.0))
                     .build(),
                 true, MathConstants<double>::kPi, Vector2d(0.0, 22.0));
  expectEndpoint(PathBuilder()
                     .moveTo(Vector2d(0.0, 0.0))
                     .curveTo(Vector2d(0.0, 0.0), Vector2d(1e-20, 0.0), Vector2d(0.0, 20.0))
                     .build(),
                 false, 0.0, Vector2d(0.0, -2.0));
  expectEndpoint(PathBuilder()
                     .moveTo(Vector2d(0.0, 0.0))
                     .curveTo(Vector2d(1e-20, 20.0), Vector2d(0.0, 20.0), Vector2d(0.0, 20.0))
                     .build(),
                 true, MathConstants<double>::kPi, Vector2d(0.0, 22.0));
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

TEST(PlacedTextGeometryTest, GlyphCellBoundsApplyGeometricVerticalScaleBeforePathWarp) {
  Registry registry;
  FontManager manager(registry);
  TextEngine engine(manager, registry);
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(5.0, 0.0))
                         .lineTo(Vector2d(5.0, 20.0))
                         .build();
  TextRun run;
  run.usedFontSizePx = 8.0f;
  TextGlyph glyph;
  glyph.glyphIndex = 1;
  glyph.textPathOrigin = 0.0;
  glyph.xAdvance = 10.0;
  glyph.stretchScaleY = 2.0f;
  run.glyphs.push_back(glyph);

  const Box2d ordinaryBounds = ComputeTextBounds(engine, {run});
  run.stretchedTextPath = std::make_shared<StretchedTextPath>(MeasurePath(route));
  const Box2d pathBounds = ComputeTextBounds(engine, {run});

  EXPECT_THAT(ordinaryBounds, BoxEq(Vector2d(0.0, -16.0), Vector2d(10.0, 0.0)));
  // Across the corner, the scaled 16-unit normal extends X to 21 while the ten-unit advance
  // reaches Y=5 on the outgoing segment.
  EXPECT_THAT(pathBounds, BoxEq(Vector2d(0.0, -16.0), Vector2d(21.0, 5.0)));
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

TEST(PlacedTextGeometryTest, StretchReverseEdgeSamplesExactSharpCornerBoundary) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path reverseEdge =
      PathBuilder().moveTo(Vector2d(15.0, -2.0)).lineTo(Vector2d(0.0, -2.0)).build();
  const Path rectangle = PathBuilder().addRect(Box2d::FromXYWH(0.0, -2.0, 15.0, 4.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;
  const StretchedTextPath mapping = MeasurePath(route);

  const Path warpedReverse = WarpGlyphOutline(reverseEdge, mapping, glyph, 0.05);
  const Path warpedRectangle = WarpGlyphOutline(rectangle, mapping, glyph, 0.05);

  ASSERT_THAT(warpedReverse.commands(), Not(IsEmpty()));
  EXPECT_THAT(warpedReverse.points(), Contains(Vector2Near(12.0, 0.0)));
  EXPECT_THAT(warpedReverse.points(), Contains(Vector2Near(10.0, -2.0)));
  EXPECT_THAT(warpedRectangle.commands(), Not(IsEmpty()));
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

  const Path turnback = PathBuilder()
                            .moveTo(Vector2d(0.0, -2.0))
                            .quadTo(Vector2d(12.0, -3.0), Vector2d(0.0, -2.0))
                            .build();
  EXPECT_FALSE(WarpGlyphOutline(turnback, MeasurePath(route), glyph, 0.05).empty());

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

TEST(PlacedTextGeometryTest, StretchCubicCrossesSharpCornerWithNormalOffset) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path outline = PathBuilder()
                           .moveTo(Vector2d(0.0, -2.0))
                           .curveTo(Vector2d(3.0, -3.0), Vector2d(7.0, -3.0), Vector2d(10.0, -2.0))
                           .build();
  const Path reverse = PathBuilder()
                           .moveTo(Vector2d(10.0, -2.0))
                           .curveTo(Vector2d(7.0, -3.0), Vector2d(3.0, -3.0), Vector2d(0.0, -2.0))
                           .build();
  const Path startsAtCorner =
      PathBuilder()
          .moveTo(Vector2d(5.0, -2.0))
          .curveTo(Vector2d(6.5, -3.0), Vector2d(8.5, -3.0), Vector2d(10.0, -2.0))
          .build();
  const Path endsAtCorner =
      PathBuilder()
          .moveTo(Vector2d(0.0, -2.0))
          .curveTo(Vector2d(1.5, -3.0), Vector2d(3.5, -3.0), Vector2d(5.0, -2.0))
          .build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  const Path warped = WarpGlyphOutline(outline, MeasurePath(route), glyph, 0.05);

  EXPECT_FALSE(warped.empty());
  EXPECT_FALSE(WarpGlyphOutline(reverse, MeasurePath(route), glyph, 0.05).empty());
  EXPECT_FALSE(WarpGlyphOutline(startsAtCorner, MeasurePath(route), glyph, 0.05).empty());
  EXPECT_FALSE(WarpGlyphOutline(endsAtCorner, MeasurePath(route), glyph, 0.05).empty());

  const Path turnback =
      PathBuilder()
          .moveTo(Vector2d(0.0, -2.0))
          .curveTo(Vector2d(12.0, -3.0), Vector2d(12.0, -3.0), Vector2d(0.0, -2.0))
          .build();
  EXPECT_FALSE(WarpGlyphOutline(turnback, MeasurePath(route), glyph, 0.05).empty());

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

TEST(PlacedTextGeometryTest, StretchCrossesConnectedZeroLengthSegmentAtCornerOnce) {
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 0.0))
                         .lineTo(Vector2d(10.0, 10.0))
                         .build();
  const Path withoutZeroLength = PathBuilder()
                                     .moveTo(Vector2d(0.0, 0.0))
                                     .lineTo(Vector2d(10.0, 0.0))
                                     .lineTo(Vector2d(10.0, 10.0))
                                     .build();
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(10.0, -2.0)).build();
  const Path reverseEdge =
      PathBuilder().moveTo(Vector2d(10.0, -2.0)).lineTo(Vector2d(0.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = 5.0;

  for (const Path* outline : {&edge, &reverseEdge}) {
    const Path warped = WarpGlyphOutline(*outline, MeasurePath(route), glyph, 0.05);
    const Path expected = WarpGlyphOutline(*outline, MeasurePath(withoutZeroLength), glyph, 0.05);

    ASSERT_FALSE(warped.empty());
    EXPECT_THAT(warped.points(), Contains(Vector2Near(10.0, -2.0)));
    EXPECT_THAT(warped.points(), Contains(Vector2Near(12.0, 0.0)));
    EXPECT_THAT(warped.points(), Contains(Vector2Near(12.0, 5.0)));
    ASSERT_EQ(warped.points().size(), expected.points().size());
    for (size_t i = 0; i < warped.points().size(); ++i) {
      EXPECT_THAT(warped.points()[i], Vector2Near(expected.points()[i].x, expected.points()[i].y));
    }
  }
}

TEST(PlacedTextGeometryTest, StretchClosedPathCrossesZeroLengthSegmentsOncePerCircuit) {
  const Path withoutZeroLength = PathBuilder()
                                     .moveTo(Vector2d(0.0, 0.0))
                                     .lineTo(Vector2d(10.0, 0.0))
                                     .lineTo(Vector2d(10.0, 10.0))
                                     .lineTo(Vector2d(0.0, 10.0))
                                     .closePath()
                                     .build();
  // A leading zero-length segment ends at distance 0, the same seam point as the closing
  // segment's end; an interior one repeats the corner at distance 10 on every circuit.
  const std::array<Path, 2> routes = {
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .lineTo(Vector2d(0.0, 0.0))
          .lineTo(Vector2d(10.0, 0.0))
          .lineTo(Vector2d(10.0, 10.0))
          .lineTo(Vector2d(0.0, 10.0))
          .closePath()
          .build(),
      PathBuilder()
          .moveTo(Vector2d(0.0, 0.0))
          .lineTo(Vector2d(10.0, 0.0))
          .lineTo(Vector2d(10.0, 0.0))
          .lineTo(Vector2d(10.0, 10.0))
          .lineTo(Vector2d(0.0, 10.0))
          .closePath()
          .build(),
  };
  StretchedTextPath expectedMapping = MeasurePath(withoutZeroLength);
  expectedMapping.closed = true;
  // Starts 5 before the seam and ends past the first corner of the next circuit.
  const Path edge = PathBuilder().moveTo(Vector2d(0.0, -2.0)).lineTo(Vector2d(20.0, -2.0)).build();
  const Path reverseEdge =
      PathBuilder().moveTo(Vector2d(20.0, -2.0)).lineTo(Vector2d(0.0, -2.0)).build();
  TextGlyph glyph;
  glyph.textPathOrigin = expectedMapping.measuredPath.pathLength() - 5.0;

  for (const Path* outline : {&edge, &reverseEdge}) {
    const Path expected = WarpGlyphOutline(*outline, expectedMapping, glyph, 0.05);
    ASSERT_FALSE(expected.empty());

    for (size_t index = 0; index < routes.size(); ++index) {
      SCOPED_TRACE(index);
      StretchedTextPath mapping = MeasurePath(routes[index]);
      mapping.closed = true;

      const Path warped = WarpGlyphOutline(*outline, mapping, glyph, 0.05);

      ASSERT_FALSE(warped.empty());
      ASSERT_EQ(warped.points().size(), expected.points().size());
      for (size_t i = 0; i < warped.points().size(); ++i) {
        EXPECT_THAT(warped.points()[i],
                    Vector2Near(expected.points()[i].x, expected.points()[i].y));
      }
    }
  }
}

TEST(PlacedTextGeometryTest, StretchClosedPathLeadingZeroLengthSeamMatchesAcrossCircuits) {
  // With 0.1 sides, `(k - 1) * length + length` and `k * length` can differ by an ulp, so on
  // some circuits the seam reached as the previous circuit's final segment end is not equal to
  // the seam reached as this circuit's leading zero-length segment end. That crossing must still
  // be emitted exactly once on every circuit.
  const Path withoutZeroLength = PathBuilder()
                                     .moveTo(Vector2d(0.0, 0.0))
                                     .lineTo(Vector2d(0.1, 0.0))
                                     .lineTo(Vector2d(0.1, 0.1))
                                     .lineTo(Vector2d(0.0, 0.1))
                                     .closePath()
                                     .build();
  const Path route = PathBuilder()
                         .moveTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(0.0, 0.0))
                         .lineTo(Vector2d(0.1, 0.0))
                         .lineTo(Vector2d(0.1, 0.1))
                         .lineTo(Vector2d(0.0, 0.1))
                         .closePath()
                         .build();
  StretchedTextPath expectedMapping = MeasurePath(withoutZeroLength);
  expectedMapping.closed = true;
  StretchedTextPath mapping = MeasurePath(route);
  mapping.closed = true;
  const double length = mapping.measuredPath.pathLength();
  ASSERT_EQ(length, expectedMapping.measuredPath.pathLength());

  const Path edge =
      PathBuilder().moveTo(Vector2d(0.0, -0.02)).lineTo(Vector2d(0.06, -0.02)).build();
  for (int circuit = 1; circuit <= 64; ++circuit) {
    SCOPED_TRACE(circuit);
    TextGlyph glyph;
    glyph.textPathOrigin = static_cast<double>(circuit) * length - 0.03;

    const Path expected = WarpGlyphOutline(edge, expectedMapping, glyph, 1e-3);
    const Path warped = WarpGlyphOutline(edge, mapping, glyph, 1e-3);

    ASSERT_FALSE(expected.empty());
    ASSERT_FALSE(warped.empty());
    ASSERT_EQ(warped.points().size(), expected.points().size());
    for (size_t i = 0; i < warped.points().size(); ++i) {
      EXPECT_THAT(warped.points()[i], Vector2Near(expected.points()[i].x, expected.points()[i].y));
    }
  }
}

TEST(PlacedTextGeometryTest, StretchExtendsOpenEndsAlongFirstAndLastNonzeroSegments) {
  // A zero-length segment at either end of an open route has no direction, so overhang before the
  // start and past the end must follow the adjacent segment that does.
  TextGlyph glyph;
  glyph.textPathOrigin = 0.0;

  const StretchedTextPath leading = MeasurePath(PathBuilder()
                                                    .moveTo(Vector2d(0.0, 0.0))
                                                    .lineTo(Vector2d(0.0, 0.0))
                                                    .lineTo(Vector2d(0.0, 20.0))
                                                    .build());
  EXPECT_THAT(MapTextPathPoint(leading, glyph, Vector2d(-1.0, -2.0)),
              Optional(Vector2Near(2.0, -1.0)));
  EXPECT_THAT(MapTextPathPoint(leading, glyph, Vector2d(0.0, -2.0)),
              Optional(Vector2Near(2.0, 0.0)));

  const StretchedTextPath trailing = MeasurePath(PathBuilder()
                                                     .moveTo(Vector2d(0.0, 0.0))
                                                     .lineTo(Vector2d(0.0, 20.0))
                                                     .lineTo(Vector2d(0.0, 20.0))
                                                     .build());
  EXPECT_THAT(MapTextPathPoint(trailing, glyph, Vector2d(21.0, -2.0)),
              Optional(Vector2Near(2.0, 21.0)));
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

TEST(PlacedTextGeometryTest, StretchToleranceUsesLinearTransformOnly) {
  const double identityTolerance = TextWarpToleranceFor(Transform2d());

  // Translation does not magnify local error.
  EXPECT_EQ(TextWarpToleranceFor(Transform2d::Translate(1000.0, 0.0)), identityTolerance);
  EXPECT_EQ(TextWarpToleranceFor(Transform2d::Translate(0.0, -1000.0)), identityTolerance);

  // Conformal transforms keep their existing tolerance.
  EXPECT_THAT(TextWarpToleranceFor(Transform2d::Rotate(MathConstants<double>::kPi / 3.0)),
              DoubleNear(identityTolerance, 1e-15));
  EXPECT_THAT(TextWarpToleranceFor(Transform2d::Scale(4.0)), DoubleNear(0.1 / 4.0, 1e-15));

  // X shear `matrix(1,0,100,1,0,0)` maps local (0, 1) to device (100, 1): its largest singular
  // value bounds the device error, so the tolerance must be at least that much tighter.
  Transform2d deviceFromLocalShear;
  deviceFromLocalShear.data[2] = 100.0;
  EXPECT_LE(TextWarpToleranceFor(deviceFromLocalShear), 0.1 / std::hypot(100.0, 1.0));
  EXPECT_GE(TextWarpToleranceFor(deviceFromLocalShear), 1e-4);

  Transform2d deviceFromLocalNonfinite;
  deviceFromLocalNonfinite.data[2] = std::numeric_limits<double>::infinity();
  EXPECT_EQ(TextWarpToleranceFor(deviceFromLocalNonfinite), 1e-4);
  deviceFromLocalNonfinite.data[2] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(TextWarpToleranceFor(deviceFromLocalNonfinite), 1e-4);

  // Magnification large enough to overflow the singular-value calculation still tightens to the
  // minimum, as does any magnification that already reaches it.
  EXPECT_EQ(TextWarpToleranceFor(Transform2d::Scale(1e200)), 1e-4);
  EXPECT_EQ(TextWarpToleranceFor(Transform2d::Scale(1000.0)), 1e-4);
  EXPECT_THAT(TextWarpToleranceFor(Transform2d::Scale(500.0)), DoubleNear(0.1 / 500.0, 1e-15));
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
