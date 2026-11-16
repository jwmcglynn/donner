#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_skia/Mask.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia/Transform.h"

using ::testing::Optional;

TEST(PathTest, ConvexClassificationAcceptsExplicitClosingVertex) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  builder.lineTo(8.0f, 2.0f);
  builder.lineTo(8.0f, 8.0f);
  builder.lineTo(2.0f, 8.0f);
  builder.lineTo(2.0f, 2.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationAcceptsClosedCurveControlPolygon) {
  constexpr float kNear = 3.34314575f;
  constexpr float kFar = 6.65685425f;
  tiny_skia::PathBuilder builder;
  builder.moveTo(8.0f, 5.0f);
  builder.cubicTo(8.0f, kFar, kFar, 8.0f, 5.0f, 8.0f);
  builder.cubicTo(kNear, 8.0f, 2.0f, kFar, 2.0f, 5.0f);
  builder.cubicTo(2.0f, kNear, kNear, 2.0f, 5.0f, 2.0f);
  builder.cubicTo(kFar, 2.0f, 8.0f, kNear, 8.0f, 5.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationAcceptsCoincidentCurveControlPoints) {
  constexpr float kNear = 3.34314575f;
  constexpr float kFar = 6.65685425f;
  tiny_skia::PathBuilder builder;
  builder.moveTo(8.0f, 5.0f);
  // A control point on its own endpoint adds a zero-length control edge, not a revisit.
  builder.cubicTo(8.0f, 5.0f, kFar, 8.0f, 5.0f, 8.0f);
  builder.cubicTo(kNear, 8.0f, 2.0f, kFar, 2.0f, 5.0f);
  builder.cubicTo(2.0f, kNear, kNear, 2.0f, 5.0f, 2.0f);
  builder.cubicTo(kFar, 2.0f, 8.0f, 5.0f, 8.0f, 5.0f);
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationRejectsRepeatedContour) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsFalse());
  const auto rectangle = tiny_skia::Rect::fromLTRB(2.0f, 2.0f, 8.0f, 8.0f);
  ASSERT_THAT(rectangle, Optional(testing::_));
  EXPECT_THAT(tiny_skia::Path::fromRect(*rectangle).isConvex(), testing::IsTrue());
}

TEST(PathTest, ConvexClassificationRejectsRepeatedExplicitlyClosedContour) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    builder.lineTo(2.0f, 2.0f);
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  EXPECT_THAT(path->isConvex(), testing::IsFalse());
}

TEST(MaskTest, RepeatedContourRespectsWindingRule) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  for (int lap = 0; lap < 2; ++lap) {
    builder.lineTo(8.0f, 2.0f);
    builder.lineTo(8.0f, 8.0f);
    builder.lineTo(2.0f, 8.0f);
    if (lap == 0) {
      builder.lineTo(2.0f, 2.0f);
    }
  }
  builder.close();
  const auto path = builder.finish();
  ASSERT_THAT(path, Optional(testing::_));
  for (const bool antiAlias : {false, true}) {
    for (const auto rule : {tiny_skia::FillRule::Winding, tiny_skia::FillRule::EvenOdd}) {
      SCOPED_TRACE(antiAlias);
      SCOPED_TRACE(static_cast<int>(rule));
      auto mask = tiny_skia::Mask::fromSize(10, 10);
      ASSERT_THAT(mask, Optional(testing::_));
      mask->fillPath(*path, rule, antiAlias, tiny_skia::Transform::identity());
      std::vector<std::uint8_t> expected(100, 0);
      if (rule == tiny_skia::FillRule::Winding) {
        for (std::size_t y = 2; y < 8; ++y) {
          for (std::size_t x = 2; x < 8; ++x) {
            expected[y * 10 + x] = 255;
          }
        }
      }
      EXPECT_THAT(mask->data(), testing::ElementsAreArray(expected));
    }
  }
}

TEST(PathTest, ConvexClassificationRejectsOverlapsAndTouches) {
  const std::vector<std::vector<tiny_skia::Point>> contours = {
      {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 2}, {4, 2}, {4, 6}, {0, 6}},
      {{0, 0}, {4, 0}, {2, 4}, {0, 0}, {3, 1}, {2, 3}},
  };
  for (const auto& points : contours) {
    SCOPED_TRACE(testing::PrintToString(points));
    tiny_skia::PathBuilder builder;
    builder.moveTo(points.front().x, points.front().y);
    for (std::size_t i = 1; i < points.size(); ++i) {
      builder.lineTo(points[i].x, points[i].y);
    }
    builder.close();
    const auto path = builder.finish();
    ASSERT_THAT(path, Optional(testing::_));
    EXPECT_THAT(path->isConvex(), testing::IsFalse());
  }
}

TEST(PathTest, ConvexClassificationRequiresFiniteTurns) {
  for (const float scale : {1.0f, std::numeric_limits<float>::max() / 4.0f}) {
    SCOPED_TRACE(scale);
    tiny_skia::PathBuilder builder;
    builder.moveTo(-scale, -scale);
    builder.lineTo(scale, -scale);
    builder.lineTo(0, scale);
    builder.close();
    const auto path = builder.finish();
    ASSERT_THAT(path, Optional(testing::_));
    EXPECT_THAT(path->isConvex(), testing::Eq(scale == 1.0f));
  }
}

namespace {

/// A circle of four cubics, radius 40 around (50, 50).
tiny_skia::Path CubicCircle() {
  constexpr float kKappa = 0.5522847f;
  constexpr float kCenter = 50.0f;
  constexpr float kRadius = 40.0f;
  constexpr float kHandle = kKappa * kRadius;
  tiny_skia::PathBuilder builder;
  builder.moveTo(kCenter + kRadius, kCenter);
  builder.cubicTo(kCenter + kRadius, kCenter + kHandle, kCenter + kHandle, kCenter + kRadius,
                  kCenter, kCenter + kRadius);
  builder.cubicTo(kCenter - kHandle, kCenter + kRadius, kCenter - kRadius, kCenter + kHandle,
                  kCenter - kRadius, kCenter);
  builder.cubicTo(kCenter - kRadius, kCenter - kHandle, kCenter - kHandle, kCenter - kRadius,
                  kCenter, kCenter - kRadius);
  builder.cubicTo(kCenter + kHandle, kCenter - kRadius, kCenter + kRadius, kCenter - kHandle,
                  kCenter + kRadius, kCenter);
  builder.close();
  return *builder.finish();
}

/// A path whose contour has already measured far enough that float rounding absorbs the length
/// of every measured piece of the small curves that follow: 200 trips along a 20,000-unit line
/// (a distance of 8,000,000, where a float step is 0.5) and then \p curveCount loops 0.5 units
/// tall, each subdivided into pieces about 0.1 long.
tiny_skia::Path AbsorbedCurves(int curveCount) {
  tiny_skia::PathBuilder builder;
  builder.moveTo(0.0f, 0.0f);
  for (int trip = 0; trip < 200; ++trip) {
    builder.lineTo(20000.0f, 0.0f);
    builder.lineTo(0.0f, 0.0f);
  }
  for (int curve = 0; curve < curveCount; ++curve) {
    builder.cubicTo(0.0f, 0.5f, 0.1f, 0.5f, 0.1f, 0.0f);
    builder.cubicTo(0.1f, -0.5f, 0.0f, -0.5f, 0.0f, 0.0f);
  }
  return *builder.finish();
}

/// The `4 4` dash pattern the dash tests use.
tiny_skia::StrokeDash FourFourDash() {
  return *tiny_skia::StrokeDash::create({4.0f, 4.0f}, 0.0f);
}

}  // namespace

TEST(PathTest, StrokeStopsAtTheOutputLimit) {
  // A limit below the outline a stroke emits stops the expansion and produces no outline; a limit
  // at it changes nothing.
  const tiny_skia::Path circle = CubicCircle();
  tiny_skia::Stroke stroke;
  stroke.width = 2.0f;

  const std::optional<tiny_skia::Path> unlimited = circle.stroke(stroke, 1000.0f);
  ASSERT_THAT(unlimited, Optional(testing::_));
  const std::size_t points = unlimited->points().size();

  tiny_skia::StrokeLimits tight{.maxOutlinePoints = points - 1};
  EXPECT_THAT(circle.stroke(stroke, 1000.0f, &tight), testing::Eq(std::nullopt));
  EXPECT_TRUE(tight.exceeded);

  tiny_skia::StrokeLimits exact{.maxOutlinePoints = points};
  const std::optional<tiny_skia::Path> outline = circle.stroke(stroke, 1000.0f, &exact);
  ASSERT_THAT(outline, Optional(testing::_));
  EXPECT_FALSE(exact.exceeded);
  EXPECT_EQ(exact.outlinePoints, points);
  EXPECT_EQ(outline->points().size(), points);
}

TEST(PathTest, StrokeOutputLimitHoldsForTheFinishedOutline) {
  // The last contour's caps, its closing join, and the inner side appended when a contour is
  // finished are all emitted after the stroker's last check inside the segment loop. The limit
  // still holds for the outline returned.
  tiny_skia::Stroke roundCaps;
  roundCaps.width = 2.0f;
  roundCaps.lineCap = tiny_skia::LineCap::Round;
  tiny_skia::PathBuilder open;
  open.moveTo(0.0f, 0.0f);
  open.lineTo(10.0f, 0.0f);
  const tiny_skia::Path line = *open.finish();

  tiny_skia::Stroke miterJoins;
  miterJoins.width = 2.0f;
  tiny_skia::PathBuilder closed;
  closed.moveTo(0.0f, 0.0f);
  closed.lineTo(10.0f, 0.0f);
  closed.lineTo(10.0f, 10.0f);
  closed.close();
  const tiny_skia::Path triangle = *closed.finish();

  for (const auto& [path, stroke] : {std::pair{line, roundCaps}, std::pair{triangle, miterJoins}}) {
    const std::optional<tiny_skia::Path> full = path.stroke(stroke, 1.0f);
    ASSERT_THAT(full, Optional(testing::_));
    const std::size_t points = full->points().size();
    tiny_skia::StrokeLimits tight{.maxOutlinePoints = points - 1};
    EXPECT_THAT(path.stroke(stroke, 1.0f, &tight), testing::Eq(std::nullopt));
    EXPECT_TRUE(tight.exceeded);
    tiny_skia::StrokeLimits exact{.maxOutlinePoints = points};
    EXPECT_THAT(path.stroke(stroke, 1.0f, &exact), Optional(testing::_));
    EXPECT_FALSE(exact.exceeded);
    EXPECT_EQ(exact.outlinePoints, points);
  }

  tiny_skia::StrokeLimits tiny{.maxOutlinePoints = 4};
  EXPECT_THAT(line.stroke(roundCaps, 1.0f, &tiny), testing::Eq(std::nullopt));
  EXPECT_TRUE(tiny.exceeded);
}

TEST(PathTest, DashStopsAtTheMeasureLimit) {
  // Measuring a dashed curve subdivides it to the device tolerance, so the steps it takes grow
  // with the resolution scale. A limit below what the measurement takes stops it and produces no
  // dashed path; a limit at it changes nothing.
  const tiny_skia::Path circle = CubicCircle();
  const tiny_skia::StrokeDash dash = FourFourDash();
  std::size_t previous = 0;
  for (const float scale : {1.0f, 100.0f}) {
    tiny_skia::StrokeLimits measured;
    ASSERT_THAT(circle.dash(dash, scale, &measured), Optional(testing::_));
    EXPECT_THAT(measured.measureSteps, testing::Gt(previous));
    previous = measured.measureSteps;
  }
  tiny_skia::StrokeLimits ample;
  const std::optional<tiny_skia::Path> dashed = circle.dash(dash, 10000.0f, &ample);
  ASSERT_THAT(dashed, Optional(testing::_));
  EXPECT_THAT(ample.measureSteps, testing::Gt(previous));
  const std::optional<tiny_skia::Path> unlimited = circle.dash(dash, 10000.0f);
  ASSERT_THAT(unlimited, Optional(testing::_));
  EXPECT_EQ(dashed->points().size(), unlimited->points().size());

  tiny_skia::StrokeLimits tight{.maxMeasureSteps = ample.measureSteps - 1};
  EXPECT_THAT(circle.dash(dash, 10000.0f, &tight), testing::Eq(std::nullopt));
  EXPECT_TRUE(tight.exceeded);
  tiny_skia::StrokeLimits exact{.maxMeasureSteps = ample.measureSteps};
  EXPECT_THAT(circle.dash(dash, 10000.0f, &exact), Optional(testing::_));
  EXPECT_FALSE(exact.exceeded);
}

TEST(PathTest, DashMeasureLimitCountsStepsWhoseLengthIsAbsorbed) {
  // Every measured piece of the small curves adds a length that float rounding absorbs into the
  // contour's distance, so none of them becomes a measured segment, yet measuring them still takes
  // time. The limit counts each step, so these curves pass a limit far below the steps they take.
  constexpr int kCurveCount = 1000;
  const tiny_skia::Path path = AbsorbedCurves(kCurveCount);
  const std::optional<tiny_skia::StrokeDash> dash =
      tiny_skia::StrokeDash::create({100000.0f, 1.0f}, 0.0f);
  ASSERT_THAT(dash, Optional(testing::_));

  tiny_skia::StrokeLimits ample;
  ASSERT_THAT(path.dash(*dash, 64.0f, &ample), Optional(testing::_));
  // More than one step per curve beyond the 400 lines.
  EXPECT_THAT(ample.measureSteps, testing::Gt(400u + 2u * 2u * kCurveCount));

  tiny_skia::StrokeLimits tight{.maxMeasureSteps = 2000};
  EXPECT_THAT(path.dash(*dash, 64.0f, &tight), testing::Eq(std::nullopt));
  EXPECT_TRUE(tight.exceeded);
}

TEST(PathTest, DashMeasureLimitIsSharedByEveryContour) {
  // The limit bounds the dash as a whole: two copies of a contour, which each fit it alone, pass
  // it together.
  const tiny_skia::Path circle = CubicCircle();
  const tiny_skia::StrokeDash dash = FourFourDash();
  tiny_skia::StrokeLimits one;
  ASSERT_THAT(circle.dash(dash, 100.0f, &one), Optional(testing::_));

  tiny_skia::PathBuilder builder;
  builder.pushPath(circle);
  builder.pushPath(circle);
  const tiny_skia::Path twoCircles = *builder.finish();
  tiny_skia::StrokeLimits both{.maxMeasureSteps = one.measureSteps + one.measureSteps / 2};
  EXPECT_THAT(twoCircles.dash(dash, 100.0f, &both), testing::Eq(std::nullopt));
  EXPECT_TRUE(both.exceeded);

  tiny_skia::StrokeLimits ample;
  ASSERT_THAT(twoCircles.dash(dash, 100.0f, &ample), Optional(testing::_));
  EXPECT_EQ(ample.measureSteps, 2 * one.measureSteps);
}

TEST(PathTest, DashMeasureLimitHoldsForAContourThatEndsTheDash) {
  // A contour whose length overflows to infinity ends the dash once it is measured, without
  // reaching the dashing loop. When the step that passes the limit falls in that contour, after
  // its length has overflowed, the dash still returns nothing, although every contour the loop
  // saw fit.
  tiny_skia::PathBuilder builder;
  builder.pushPath(CubicCircle());
  builder.moveTo(-3.0e38f, 0.0f);
  builder.lineTo(3.0e38f, 0.0f);
  builder.lineTo(0.0f, 0.0f);
  const tiny_skia::Path path = *builder.finish();
  const tiny_skia::StrokeDash dash = FourFourDash();

  tiny_skia::StrokeLimits ample;
  ASSERT_THAT(path.dash(dash, 1.0f, &ample), Optional(testing::_));
  ASSERT_THAT(ample.measureSteps, testing::Gt(1u));

  tiny_skia::StrokeLimits allButLast{.maxMeasureSteps = ample.measureSteps - 1};
  EXPECT_THAT(path.dash(dash, 1.0f, &allButLast), testing::Eq(std::nullopt));
  EXPECT_TRUE(allButLast.exceeded);
}

TEST(PathTest, ExceededLimitsStopLaterDashesAndStrokes) {
  // Once a limit is exceeded, nothing more is dashed or stroked under it.
  const tiny_skia::Path circle = CubicCircle();
  tiny_skia::Stroke stroke;
  stroke.width = 2.0f;
  tiny_skia::StrokeLimits limits{.exceeded = true};
  EXPECT_THAT(circle.dash(FourFourDash(), 1.0f, &limits), testing::Eq(std::nullopt));
  EXPECT_THAT(circle.stroke(stroke, 1.0f, &limits), testing::Eq(std::nullopt));
  EXPECT_EQ(limits.measureSteps, 0u);
  EXPECT_EQ(limits.outlinePoints, 0u);
}

TEST(PathTest, MiterLimitDoesNotLowerTheStrokeResolution) {
  // The precision floor depends on the coordinates the curve offsetting works in, which stay
  // within the stroke radius of the path, not on the miter limit: a curve without corners strokes
  // to the same outline under any miter limit.
  const tiny_skia::Path circle = CubicCircle();
  tiny_skia::Stroke usual;
  usual.width = 10.0f;
  tiny_skia::Stroke huge = usual;
  huge.miterLimit = 1.0e9f;
  for (const float scale : {1.0f, 100.0f}) {
    SCOPED_TRACE(scale);
    const std::optional<tiny_skia::Path> expected = circle.stroke(usual, scale);
    const std::optional<tiny_skia::Path> actual = circle.stroke(huge, scale);
    ASSERT_THAT(expected, Optional(testing::_));
    ASSERT_THAT(actual, Optional(testing::_));
    EXPECT_THAT(actual->points(), testing::ElementsAreArray(expected->points()));
  }
}

TEST(PathTest, ResolutionScaleFloorHandlesTheLargestFloat) {
  // The largest finite float has no float step above it; the floor still yields a positive scale.
  const float scale =
      tiny_skia::PrecisionLimitedResScale(1.0f, std::numeric_limits<float>::max(), 0.25f);
  EXPECT_THAT(scale, testing::AllOf(testing::Gt(0.0f), testing::Le(1.0f)));
}

TEST(PathTest, ResolutionScaleStaysAboveCoordinatePrecision) {
  // At ordinary scales the resolution scale is unchanged. Far past the point where the curve
  // tolerance falls under the precision of the coordinates, it stops growing.
  EXPECT_EQ(tiny_skia::PrecisionLimitedResScale(1.0f, 100.0f, 0.25f), 1.0f);
  EXPECT_EQ(tiny_skia::PrecisionLimitedResScale(1000.0f, 100.0f, 0.25f), 1000.0f);
  const float limited = tiny_skia::PrecisionLimitedResScale(1.0e9f, 100.0f, 0.25f);
  EXPECT_THAT(limited, testing::AllOf(testing::Gt(1000.0f), testing::Lt(1.0e5f)));
  EXPECT_EQ(tiny_skia::PrecisionLimitedResScale(1.0e12f, 100.0f, 0.25f), limited);
  // Larger coordinates have coarser precision and a lower bound.
  EXPECT_THAT(tiny_skia::PrecisionLimitedResScale(1.0e9f, 10000.0f, 0.25f), testing::Lt(limited));
}

TEST(PathTest, StrokeStaysWithinOneFloatStepWhenTheFloorEngages) {
  // A circle of radius 40 around (4,000,000, 4,000,000), where a float step is 0.25, stroked 10
  // wide at a resolution scale of 100, past where the floor engages. The floor's tolerance is the
  // error allowed where the outline's quads bend away from the circles 35 and 45 from the center,
  // so their midpoints stay within one float step of those circles; with a tolerance of four
  // steps they would not. The quads' end points, which also carry the rounding of the circle's
  // control points to floats, stay within two steps.
  constexpr double kCenter = 4.0e6;
  constexpr float kStep = 0.25f;
  constexpr float kScale = 100.0f;
  ASSERT_EQ(std::nextafter(static_cast<float>(kCenter), 5.0e6f) - static_cast<float>(kCenter),
            kStep);
  ASSERT_THAT(
      tiny_skia::PrecisionLimitedResScale(kScale, static_cast<float>(kCenter) + 60.0f, 0.25f),
      testing::Lt(kScale));

  const tiny_skia::Path circle = *CubicCircle().transform(tiny_skia::Transform::fromTranslate(
      static_cast<float>(kCenter - 50.0), static_cast<float>(kCenter - 50.0)));
  tiny_skia::Stroke stroke;
  stroke.width = 10.0f;
  const std::optional<tiny_skia::Path> outline = circle.stroke(stroke, kScale);
  ASSERT_THAT(outline, Optional(testing::_));

  // Distance from the nearer of the two circles the outline follows.
  const auto offCircle = [&](double x, double y) {
    const double radius = std::hypot(x - kCenter, y - kCenter);
    return std::min(std::abs(radius - 35.0), std::abs(radius - 45.0));
  };
  const auto points = outline->points();
  std::size_t index = 0;
  std::size_t quads = 0;
  for (const tiny_skia::PathVerb verb : outline->verbs()) {
    switch (verb) {
      case tiny_skia::PathVerb::Move:
      case tiny_skia::PathVerb::Line:
        EXPECT_THAT(offCircle(points[index].x, points[index].y), testing::Le(2.0 * kStep));
        index += 1;
        break;
      case tiny_skia::PathVerb::Quad: {
        const tiny_skia::Point start = points[index - 1];
        const tiny_skia::Point control = points[index];
        const tiny_skia::Point end = points[index + 1];
        EXPECT_THAT(offCircle(0.25 * start.x + 0.5 * control.x + 0.25 * end.x,
                              0.25 * start.y + 0.5 * control.y + 0.25 * end.y),
                    testing::Le(kStep))
            << "quad ending at " << end.x << ", " << end.y;
        EXPECT_THAT(offCircle(end.x, end.y), testing::Le(2.0 * kStep));
        index += 2;
        ++quads;
        break;
      }
      case tiny_skia::PathVerb::Cubic: index += 3; break;
      case tiny_skia::PathVerb::Close: break;
    }
  }
  EXPECT_THAT(quads, testing::Gt(8u));
}

TEST(PathTest, PrecisionFloorIsPerSegment) {
  // A path that holds a short line four million units out and a small curve near the origin. The
  // curve's tolerance depends only on its own coordinates, so its stroke outline and its dash
  // measurement are those of the curve stroked or dashed alone.
  constexpr float kScale = 20.0f;
  tiny_skia::PathBuilder curveBuilder;
  curveBuilder.moveTo(1.0f, 1.0f);
  curveBuilder.cubicTo(1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f);
  const tiny_skia::Path curve = *curveBuilder.finish();
  tiny_skia::PathBuilder lineBuilder;
  lineBuilder.moveTo(4000000.0f, 0.0f);
  lineBuilder.lineTo(4000001.0f, 0.0f);
  const tiny_skia::Path line = *lineBuilder.finish();
  tiny_skia::PathBuilder bothBuilder;
  bothBuilder.pushPath(line);
  bothBuilder.pushPath(curve);
  const tiny_skia::Path both = *bothBuilder.finish();

  tiny_skia::Stroke stroke;
  stroke.width = 0.5f;
  const std::optional<tiny_skia::Path> curveOutline = curve.stroke(stroke, kScale);
  const std::optional<tiny_skia::Path> bothOutline = both.stroke(stroke, kScale);
  ASSERT_THAT(curveOutline, Optional(testing::_));
  ASSERT_THAT(bothOutline, Optional(testing::_));
  // The curve's contours come last.
  const auto curvePoints = curveOutline->points();
  const auto bothPoints = bothOutline->points();
  ASSERT_THAT(bothPoints.size(), testing::Gt(curvePoints.size()));
  EXPECT_THAT(bothPoints.last(curvePoints.size()), testing::ElementsAreArray(curvePoints));

  const std::optional<tiny_skia::StrokeDash> dash =
      tiny_skia::StrokeDash::create({0.1f, 0.1f}, 0.0f);
  ASSERT_THAT(dash, Optional(testing::_));
  tiny_skia::StrokeLimits curveSteps;
  tiny_skia::StrokeLimits lineSteps;
  tiny_skia::StrokeLimits bothSteps;
  const std::optional<tiny_skia::Path> curveDashes = curve.dash(*dash, kScale, &curveSteps);
  ASSERT_THAT(line.dash(*dash, kScale, &lineSteps), Optional(testing::_));
  const std::optional<tiny_skia::Path> bothDashes = both.dash(*dash, kScale, &bothSteps);
  ASSERT_THAT(curveDashes, Optional(testing::_));
  ASSERT_THAT(bothDashes, Optional(testing::_));
  EXPECT_EQ(bothSteps.measureSteps, lineSteps.measureSteps + curveSteps.measureSteps);
  const auto curveDashPoints = curveDashes->points();
  EXPECT_THAT(bothDashes->points().last(curveDashPoints.size()),
              testing::ElementsAreArray(curveDashPoints));
}

TEST(PathTest, DashMeasuresACurveWithALargeConstantCoordinate) {
  // A quad four million units out whose x never changes, rising 0.2 and returning, measured at a
  // resolution scale of 1,000. Only its y moves, and y is small, so it measures as it does at the
  // origin, 0.4 long, and dashes to the same dashes moved four million units.
  const auto quadAt = [](float x) {
    tiny_skia::PathBuilder builder;
    builder.moveTo(x, 0.0f);
    builder.quadTo(x, 0.4f, x, 0.0f);
    return *builder.finish();
  };
  const std::optional<tiny_skia::StrokeDash> dash =
      tiny_skia::StrokeDash::create({0.05f, 0.05f}, 0.0f);
  ASSERT_THAT(dash, Optional(testing::_));
  const std::optional<tiny_skia::Path> near = quadAt(0.0f).dash(*dash, 1000.0f);
  const std::optional<tiny_skia::Path> far = quadAt(4000000.0f).dash(*dash, 1000.0f);
  ASSERT_THAT(near, Optional(testing::_));
  ASSERT_THAT(far, Optional(testing::_));
  std::vector<tiny_skia::Point> moved(near->points().begin(), near->points().end());
  for (tiny_skia::Point& point : moved) {
    point.x += 4000000.0f;
  }
  EXPECT_THAT(far->points(), testing::ElementsAreArray(moved));
}

TEST(PathTest, StrokeAndDashWorkStaysBoundedAtExtremeScales) {
  // Past the precision of the coordinates, every larger resolution scale subdivides to the same
  // tolerance: the stroke's outline and the dash's measurement steps are the same at 10^6 and 10^9,
  // and the stroke has an outline.
  const tiny_skia::Path circle = CubicCircle();
  tiny_skia::Stroke stroke;
  stroke.width = 2.0f;
  const std::optional<tiny_skia::Path> atMillion = circle.stroke(stroke, 1.0e6f);
  const std::optional<tiny_skia::Path> atBillion = circle.stroke(stroke, 1.0e9f);
  ASSERT_THAT(atMillion, Optional(testing::_));
  ASSERT_THAT(atBillion, Optional(testing::_));
  EXPECT_THAT(atBillion->points(), testing::ElementsAreArray(atMillion->points()));
  EXPECT_THAT(atBillion->points().size(), testing::Lt(16384u));

  const tiny_skia::StrokeDash dash = FourFourDash();
  tiny_skia::StrokeLimits million;
  tiny_skia::StrokeLimits billion;
  ASSERT_THAT(circle.dash(dash, 1.0e6f, &million), Optional(testing::_));
  ASSERT_THAT(circle.dash(dash, 1.0e9f, &billion), Optional(testing::_));
  EXPECT_EQ(million.measureSteps, billion.measureSteps);
  EXPECT_THAT(billion.measureSteps, testing::Lt(65536u));
}
