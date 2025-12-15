#include "donner/backends/tiny_skia_cpp/PathGeometry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

using svg::PathSpline;
using ::testing::AllOf;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FloatNear;
using ::testing::Matcher;

Matcher<PathPoint> PointIs(float x, float y) {
  return AllOf(Field("x", &PathPoint::x, FloatNear(x, 1e-5f)),
               Field("y", &PathPoint::y, FloatNear(y, 1e-5f)));
}

Matcher<Vector2d> VectorIs(double x, double y) {
  return AllOf(Field("x", &Vector2d::x, DoubleNear(x, 1e-6)),
               Field("y", &Vector2d::y, DoubleNear(y, 1e-6)));
}

PathSpline BuildSimpleSpline() {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});
  spline.curveTo({15.0, 5.0}, {20.0, 5.0}, {25.0, 0.0});
  spline.closePath();
  return spline;
}

TEST(PathIteratorTests, IteratesCommandsInOrder) {
  PathSpline spline = BuildSimpleSpline();
  PathIterator iterator(spline);

  std::vector<PathSegment> segments;
  for (std::optional<PathSegment> segment = iterator.next(); segment.has_value();
       segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 4u);

  EXPECT_EQ(segments[0].verb, PathVerb::kMove);
  EXPECT_EQ(segments[0].pointCount, 1u);
  EXPECT_THAT(segments[0].points[0], PointIs(0.0f, 0.0f));

  EXPECT_EQ(segments[1].verb, PathVerb::kLine);
  EXPECT_EQ(segments[1].pointCount, 1u);
  EXPECT_THAT(segments[1].points[0], PointIs(10.0f, 0.0f));

  EXPECT_EQ(segments[2].verb, PathVerb::kCubic);
  EXPECT_EQ(segments[2].pointCount, 3u);
  EXPECT_THAT(segments[2].points[0], PointIs(15.0f, 5.0f));
  EXPECT_THAT(segments[2].points[1], PointIs(20.0f, 5.0f));
  EXPECT_THAT(segments[2].points[2], PointIs(25.0f, 0.0f));

  EXPECT_EQ(segments[3].verb, PathVerb::kClose);
  EXPECT_EQ(segments[3].pointCount, 1u);
  EXPECT_THAT(segments[3].points[0], PointIs(0.0f, 0.0f));
}

TEST(PathIteratorTests, MarksInternalArcPoints) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.arcTo({5.0, 5.0}, 0.0, false, true, {10.0, 0.0});

  PathIterator iterator(spline);
  std::vector<PathSegment> segments;
  while (auto segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_GE(segments.size(), 2u);
  const bool hasInternalArc =
      std::any_of(segments.begin() + 1, segments.end(),
                  [](const PathSegment& segment) { return segment.isInternalPoint; });
  EXPECT_TRUE(hasInternalArc);
}

TEST(ComputeBoundingBoxTests, IncludesCurveExtrema) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.curveTo({10.0, 20.0}, {20.0, -20.0}, {30.0, 0.0});

  std::optional<Boxd> bounds = ComputeBoundingBox(spline);
  ASSERT_TRUE(bounds.has_value());
  EXPECT_THAT(bounds->topLeft, VectorIs(0.0, -5.7735026919));
  EXPECT_THAT(bounds->bottomRight, VectorIs(30.0, 5.7735026919));
}

TEST(ComputeBoundingBoxTests, HandlesCompositePath) {
  PathSpline spline = BuildSimpleSpline();
  std::optional<Boxd> bounds = ComputeBoundingBox(spline);

  ASSERT_TRUE(bounds.has_value());
  EXPECT_THAT(bounds->topLeft, VectorIs(0.0, 0.0));
  EXPECT_THAT(bounds->bottomRight, VectorIs(25.0, 3.75));
}

TEST(DashBuilderTests, DashesSimpleLinePath) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});

  auto dash = StrokeDash::Create({3.0f, 2.0f}, 0.0f);
  ASSERT_TRUE(dash.has_value());

  PathSpline dashed = ApplyDash(spline, *dash);
  PathIterator iterator(dashed);

  std::vector<PathSegment> segments;
  for (std::optional<PathSegment> segment = iterator.next(); segment.has_value();
       segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 4u);
  EXPECT_EQ(segments[0].verb, PathVerb::kMove);
  EXPECT_THAT(segments[0].points[0], PointIs(0.0f, 0.0f));
  EXPECT_EQ(segments[1].verb, PathVerb::kLine);
  EXPECT_THAT(segments[1].points[0], PointIs(3.0f, 0.0f));
  EXPECT_EQ(segments[2].verb, PathVerb::kMove);
  EXPECT_THAT(segments[2].points[0], PointIs(5.0f, 0.0f));
  EXPECT_EQ(segments[3].verb, PathVerb::kLine);
  EXPECT_THAT(segments[3].points[0], PointIs(8.0f, 0.0f));
}

TEST(DashBuilderTests, ResetsPerSubpathAndContinuesAcrossSegments) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});
  spline.lineTo({10.0, 10.0});
  spline.moveTo({20.0, 0.0});
  spline.lineTo({30.0, 0.0});

  auto dash = StrokeDash::Create({4.0f, 2.0f}, 0.0f);
  ASSERT_TRUE(dash.has_value());

  PathSpline dashed = ApplyDash(spline, *dash);
  PathIterator iterator(dashed);

  std::vector<PathSegment> segments;
  for (std::optional<PathSegment> segment = iterator.next(); segment.has_value();
       segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 12u);
  EXPECT_THAT(segments[0].points[0], PointIs(0.0f, 0.0f));
  EXPECT_THAT(segments[1].points[0], PointIs(4.0f, 0.0f));
  EXPECT_THAT(segments[2].points[0], PointIs(6.0f, 0.0f));
  EXPECT_THAT(segments[3].points[0], PointIs(10.0f, 0.0f));
  EXPECT_THAT(segments[4].points[0], PointIs(10.0f, 2.0f));
  EXPECT_THAT(segments[5].points[0], PointIs(10.0f, 6.0f));
  EXPECT_THAT(segments[6].points[0], PointIs(10.0f, 8.0f));
  EXPECT_THAT(segments[7].points[0], PointIs(10.0f, 10.0f));
  EXPECT_THAT(segments[8].points[0], PointIs(20.0f, 0.0f));
  EXPECT_THAT(segments[9].points[0], PointIs(24.0f, 0.0f));
  EXPECT_THAT(segments[10].points[0], PointIs(26.0f, 0.0f));
  EXPECT_THAT(segments[11].points[0], PointIs(30.0f, 0.0f));
}

TEST(StrokeBuilderTests, BuildsButtRectangleForSimpleLine) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kButt;
  stroke.lineJoin = LineJoin::kMiter;

  PathSpline outlined = ApplyStroke(spline, stroke);
  PathIterator iterator(outlined);

  std::vector<PathSegment> segments;
  while (auto segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 5u);
  EXPECT_THAT(segments[0].points[0], PointIs(0.0f, 1.0f));
  EXPECT_THAT(segments[1].points[0], PointIs(10.0f, 1.0f));
  EXPECT_THAT(segments[2].points[0], PointIs(10.0f, -1.0f));
  EXPECT_THAT(segments[3].points[0], PointIs(0.0f, -1.0f));
}

TEST(StrokeBuilderTests, ExtendsSquareCapsBeyondEndpoints) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kSquare;

  PathSpline outlined = ApplyStroke(spline, stroke);
  PathIterator iterator(outlined);

  std::vector<PathSegment> segments;
  while (auto segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 5u);
  EXPECT_THAT(segments[0].points[0], PointIs(-1.0f, 1.0f));
  EXPECT_THAT(segments[1].points[0], PointIs(11.0f, 1.0f));
  EXPECT_THAT(segments[2].points[0], PointIs(11.0f, -1.0f));
  EXPECT_THAT(segments[3].points[0], PointIs(-1.0f, -1.0f));
}

TEST(StrokeBuilderTests, BuildsRoundCapsCenteredOnEndpoints) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kRound;

  PathSpline outlined = ApplyStroke(spline, stroke);
  PathIterator iterator(outlined);

  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();

  while (auto segment = iterator.next()) {
    for (size_t i = 0; i < segment->pointCount; ++i) {
      minX = std::min(minX, segment->points[i].x);
      minY = std::min(minY, segment->points[i].y);
      maxX = std::max(maxX, segment->points[i].x);
      maxY = std::max(maxY, segment->points[i].y);
    }
  }

  EXPECT_THAT(minX, FloatNear(-1.0f, 1e-5f));
  EXPECT_THAT(maxX, FloatNear(11.0f, 1e-5f));
  EXPECT_THAT(minY, FloatNear(-1.0f, 1e-5f));
  EXPECT_THAT(maxY, FloatNear(1.0f, 1e-5f));
}

TEST(StrokeBuilderTests, BevelFallbackWhenMiterExceedsLimit) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});
  spline.lineTo({10.0, 10.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineJoin = LineJoin::kMiter;
  stroke.miterLimit = 1.0f;

  PathSpline outlined = ApplyStroke(spline, stroke);
  PathIterator iterator(outlined);

  std::vector<PathSegment> segments;
  while (auto segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_GE(segments.size(), 3u);
  EXPECT_THAT(segments[1].points[0], PointIs(10.0f, 1.0f));
  EXPECT_THAT(segments[2].points[0], PointIs(9.0f, 0.0f));
}

TEST(StrokeBuilderTests, HandlesClosedPolygonsWithoutDroppingWrapJoin) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});
  spline.lineTo({10.0, 10.0});
  spline.lineTo({0.0, 10.0});
  spline.closePath();

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineJoin = LineJoin::kMiter;

  PathSpline outlined = ApplyStroke(spline, stroke);
  PathIterator iterator(outlined);

  std::vector<PathSegment> segments;
  while (auto segment = iterator.next()) {
    segments.push_back(*segment);
  }

  ASSERT_EQ(segments.size(), 9u);
  ASSERT_EQ(segments[0].verb, PathVerb::kMove);
  EXPECT_THAT(segments[0].points[0], PointIs(1.0f, 1.0f));
  EXPECT_THAT(segments[1].points[0], PointIs(9.0f, 1.0f));
  EXPECT_THAT(segments[2].points[0], PointIs(9.0f, 9.0f));
  EXPECT_THAT(segments[3].points[0], PointIs(1.0f, 9.0f));
  EXPECT_THAT(segments[4].points[0], PointIs(-1.0f, 11.0f));
  EXPECT_THAT(segments[5].points[0], PointIs(11.0f, 11.0f));
  EXPECT_THAT(segments[6].points[0], PointIs(11.0f, -1.0f));
  EXPECT_THAT(segments[7].points[0], PointIs(-1.0f, -1.0f));
  EXPECT_EQ(segments[8].verb, PathVerb::kClose);
}

TEST(ComputeStrokeBoundsTests, ExpandsLineForSquareCap) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({10.0, 0.0});

  Stroke stroke;
  stroke.width = 4.0f;
  stroke.lineCap = LineCap::kSquare;

  const std::optional<Boxd> bounds = ComputeStrokeBounds(spline, stroke);

  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->topLeft, Vector2d(-2.0, -2.0));
  EXPECT_EQ(bounds->bottomRight, Vector2d(12.0, 2.0));
}

TEST(ComputeStrokeBoundsTests, IncludesRoundCapsForVerticalLine) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({0.0, 10.0});

  Stroke stroke;
  stroke.width = 6.0f;
  stroke.lineCap = LineCap::kRound;

  const std::optional<Boxd> bounds = ComputeStrokeBounds(spline, stroke);

  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->topLeft, Vector2d(-3.0, -3.0));
  EXPECT_EQ(bounds->bottomRight, Vector2d(3.0, 13.0));
}

TEST(ComputeStrokeBoundsTests, ClosedPolygonUsesStrokeWidth) {
  PathSpline spline;
  spline.moveTo({0.0, 0.0});
  spline.lineTo({5.0, 0.0});
  spline.lineTo({5.0, 5.0});
  spline.lineTo({0.0, 5.0});
  spline.closePath();

  Stroke stroke;
  stroke.width = 2.0f;

  const std::optional<Boxd> bounds = ComputeStrokeBounds(spline, stroke);

  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->topLeft, Vector2d(-1.0, -1.0));
  EXPECT_EQ(bounds->bottomRight, Vector2d(6.0, 6.0));
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
