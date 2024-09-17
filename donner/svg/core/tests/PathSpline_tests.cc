#include "donner/svg/core/PathSpline.h"

#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/tests/PathSplineTestUtils.h"

using testing::_;  // NOLINT, reserved-identifier: Allow for convenience
using testing::AllOf;
using testing::DoubleEq;
using testing::DoubleNear;
using testing::ElementsAre;
using testing::Gt;
using testing::Lt;
using testing::Matcher;
using testing::SizeIs;

namespace donner::svg {

namespace {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

constexpr Vector2d kVec1(123.0, 456.7);
constexpr Vector2d kVec2(78.9, 1011.12);
constexpr Vector2d kVec3(-1314.0, 1516.17);
constexpr Vector2d kVec4(1819.0, -2021.22);

}  // namespace

TEST(PathSpline, MoveTo) {
  PathSpline spline;
  spline.moveTo(kVec1);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(CommandType::MoveTo, 0)));
}

TEST(PathSpline, MoveToMultiple) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.moveTo(kVec2);

  // Only the last moveTo is used.
  EXPECT_THAT(spline.points(), ElementsAre(kVec2));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(CommandType::MoveTo, 0)));
}

TEST(PathSpline, MoveToMultipleSegments) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.moveTo(kVec3);
  spline.lineTo(kVec4);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3}));
}

TEST(PathSpline, MoveToUnused) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.moveTo(kVec3);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}));
}

TEST(PathSpline, LineTo) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);

  // Only the last command remains.
  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
}

TEST(PathSpline, LineToComplex) {
  PathSpline spline;
  spline.moveTo(Vector2d::Zero());
  spline.lineTo(kVec1);
  // Create a separate line with two segments.
  spline.moveTo(Vector2d::Zero());
  spline.lineTo(kVec2);
  spline.lineTo(kVec1);

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d::Zero(), kVec1, Vector2d::Zero(), kVec2, kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3},
                          Command{CommandType::LineTo, 4}));
}

TEST(PathSpline, LineToFailsWithoutStart) {
  PathSpline spline;
  EXPECT_DEATH(spline.lineTo(kVec1), "without calling moveTo");
}

TEST(PathSpline, CurveTo) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.curveTo(kVec2, kVec3, kVec4);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSpline, CurveToChained) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.curveTo(kVec2, kVec3, kVec4);
  spline.curveTo(kVec1, kVec2, Vector2d::Zero());
  spline.lineTo(kVec1);

  EXPECT_THAT(spline.points(),
              ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1, kVec2, Vector2d::Zero(), kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::LineTo, 7}));
}

TEST(PathSpline, CurveToFailsWithoutStart) {
  PathSpline spline;
  EXPECT_DEATH(spline.curveTo(kVec1, kVec2, kVec3), "without calling moveTo");
}

TEST(PathSpline, ArcTo) {
  PathSpline spline;
  spline.moveTo(Vector2d(1.0, 0.0));
  spline.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
               Vector2d(0.0, 2.0));

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSpline, ArcToLargeArc) {
  PathSpline spline;
  spline.moveTo(Vector2d(1.0, 0.0));
  spline.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, true, false, Vector2d(0.0, 2.0));

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, -2.0), _, _, _,
                                           _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7}));
}

TEST(PathSpline, ClosePath) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.closePath();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}));
}

TEST(PathSpline, ClosePathNeedsModeToReopen) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.closePath();
  spline.moveTo(kVec1);
  spline.lineTo(kVec3);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec1, kVec3));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                          Command{CommandType::LineTo, 3}));
}

TEST(PathSpline, ClosePathFailsWithoutStart) {
  PathSpline spline;
  EXPECT_DEATH(spline.closePath(), "without an open path");
}

TEST(PathSpline, ClosePathAfterMoveTo) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.closePath();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
}

TEST(PathSpline, ClosePathMoveToReplace) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.closePath();
  spline.moveTo(kVec3);
  spline.lineTo(kVec4);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                          Command{CommandType::LineTo, 3}));
}

TEST(PathSpline, Ellipse) {
  PathSpline spline;
  spline.ellipse(Vector2d(0.0, 1.0), Vector2d(2.0, 1.0));

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 2.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 0.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::ClosePath, 0}));
}

TEST(PathSpline, Circle) {
  PathSpline spline;
  spline.circle(Vector2d(0.0, 1.0), 2.0);

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 3.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, -1.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::ClosePath, 0}));
}

TEST(PathSpline, Empty) {
  PathSpline spline;
  EXPECT_TRUE(spline.empty());
}

TEST(PathSpline, PathLengthEmpty) {
  PathSpline spline;
  EXPECT_EQ(spline.pathLength(), 0.0);
}

TEST(PathSpline, PathLengthSingleLine) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);

  const double expectedLength = (kVec2 - kVec1).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(PathSpline, PathLengthMultipleSegments) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.lineTo(kVec3);
  spline.lineTo(kVec4);

  const double expectedLength =
      (kVec2 - kVec1).length() + (kVec3 - kVec2).length() + (kVec4 - kVec3).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(PathSpline, PathLengthCurveTo) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.curveTo(kVec2, kVec3, kVec4);

  const double tolerance = 0.001;
  double expectedLength = 4106.97786;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthComplexPath) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.curveTo(kVec3, kVec4, Vector2d(1.0, 1.0));
  spline.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
               Vector2d(0.0, 2.0));

  SCOPED_TRACE(testing::Message() << "Path: " << spline);

  // Value is saved from a previous run, it should not change.
  const double tolerance = 0.001;
  double expectedLength = 3674.25092;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthSimpleCurve) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(1, 2), Vector2d(3, 2), Vector2d(4, 0));

  // Calculate the expected length of the simple cubic Bezier curve
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 5.26836554;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthLoop) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(1, 2), Vector2d(3, -2), Vector2d(4, 0));

  // Calculate the expected length of the cubic Bezier curve with a loop
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 4.79396527;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthCusp) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(1, 2), Vector2d(2, 2), Vector2d(3, 0));

  // Calculate the expected length of the cubic Bezier curve with a cusp
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 4.43682857;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthInflectionPoint) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(1, 2), Vector2d(2, -2), Vector2d(3, 0));

  // Calculate the expected length of the cubic Bezier curve with an inflection point
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 3.93406628;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(PathSpline, PathLengthCollinearControlPoints) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(1, 1), Vector2d(2, 2), Vector2d(3, 3));

  // For collinear control points, the curve should be a straight line
  const double expectedLength = (Vector2d(3, 3) - Vector2d(0, 0)).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(PathSpline, BoundsEmptyFails) {
  PathSpline spline;
  EXPECT_DEATH(spline.bounds(), "!empty()");
  EXPECT_DEATH(spline.strokeMiterBounds(1.0, 1.0), "!empty()");
}

TEST(PathSpline, Bounds) {
  PathSpline spline;
  spline.moveTo(Vector2d::Zero());
  spline.lineTo(kVec1);
  spline.lineTo(kVec2);

  EXPECT_EQ(spline.bounds(), Boxd(Vector2d(0.0, 0.0), Vector2d(123.0, 1011.12)));
}

TEST(PathSpline, BoundsCurve) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.curveTo(Vector2d(8.0, 9.0), Vector2d(2.0, 0.0), Vector2d(0.0, 0.0));

  EXPECT_THAT(spline.bounds(), BoxEq(Vector2d(0.0, 0.0), Vector2Near(4.04307, 4.0)));
}

TEST(PathSpline, BoundsEllipse) {
  PathSpline spline;
  spline.ellipse(Vector2d(1.0, 2.0), Vector2d(2.0, 1.0));

  EXPECT_THAT(spline.bounds(), Boxd(Vector2d(-1.0, 1.0), Vector2d(3.0, 3.0)));
}

TEST(PathSpline, StrokeMiterBounds) {
  // Line segment with top making a 60 degree angle; to simplify the math the size is 100pt tall.
  //
  //      (0, 100)
  //        /\
  //       /  \
  //      /    \    x = 100 tan(30°)
  //     /      \     = 100 / sqrt(3)
  //    /        \
  //   /          \
  //  (-x, 0)      (x, 0)

  const double kXHalfExtent = 100.0 / sqrt(3.0);
  const Vector2 kBottomLeft = Vector2(-kXHalfExtent, 0.0);
  const Vector2 kBottomRight = Vector2(kXHalfExtent, 0.0);

  PathSpline spline;
  spline.moveTo(kBottomLeft);
  spline.lineTo(Vector2(0.0, 100.0));
  spline.lineTo(kBottomRight);

  SCOPED_TRACE(testing::Message() << "Path: " << spline);

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Boxd kBoundsWithoutMiter = Boxd(kBottomLeft, Vector2d(kXHalfExtent, 100.0));
  // The expected cutoff for stroke width 5 is: c=5/sin(60deg/2), giving c=10.0
  const double kExpectedCutoff = 10.0;

  // Simple bounds should not include miter.
  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  // A low cutoff is equivalent to bounds().
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);

  // At a high cutoff, there is a joint.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0),
              BoxEq(kBottomLeft, Vector2Eq(kXHalfExtent, DoubleNear(110.0, 0.01))));

  // Test right above the cutoff.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff + 0.1),
              BoxEq(kBottomLeft, Vector2Eq(kXHalfExtent, DoubleNear(110.0, 0.01))));

  // Test below the cutoff.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff - 0.1), kBoundsWithoutMiter);
}

TEST(PathSpline, StrokeMiterBoundsClosePath) {
  // Like StrokeMiterBounds, except with ClosePath called completing the triangle.
  //
  //      (0, 100)
  //        /\
  //       /  \
  //      /    \    x = 100 tan(30°)
  //     /      \     = 100 / sqrt(3)
  //    /        \
  //   /__________\
  //  (-x, 0)      (x, 0)

  const double kXHalfExtent = 100.0 / sqrt(3.0);
  const Vector2 kBottomLeft = Vector2(-kXHalfExtent, 0.0);
  const Vector2 kBottomRight = Vector2(kXHalfExtent, 0.0);

  PathSpline spline;
  spline.moveTo(kBottomLeft);
  spline.lineTo(Vector2(0.0, 100.0));
  spline.lineTo(kBottomRight);
  spline.closePath();

  ASSERT_THAT(spline.commands(), SizeIs(4));

  const Boxd kBoundsWithoutMiter = Boxd(kBottomLeft, Vector2d(kXHalfExtent, 100.0));
  // The expected cutoff for stroke width 5 is: c=5/sin(60deg/2), giving c=10.0
  const double kExpectedCutoff = 10.0;

  // Simple bounds should not include miter.
  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  // A low cutoff is equivalent to bounds().
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);

  // At a high cutoff, there is a joint for all three sides.
  const double kBottomMiterX = 8.66027;
  auto matchSizeWithMiter = BoxEq(Vector2Near(-kXHalfExtent - kBottomMiterX, -5.0),
                                  Vector2Near(kXHalfExtent + kBottomMiterX, 110.0));

  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), matchSizeWithMiter);

  // Test right above the cutoff.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff + 0.1), matchSizeWithMiter);

  // Test below the cutoff.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff - 0.1), kBoundsWithoutMiter);
}

TEST(PathSpline, StrokeMiterBoundsColinear) {
  // Two line segments that have the same tangent.
  //
  //   .-------->.-------->.
  // (0, 0)   (50, 0)   (100, 0)
  //
  PathSpline spline;
  spline.moveTo(Vector2d::Zero());
  spline.lineTo(Vector2d(0.0, 50.0));
  spline.lineTo(Vector2d(0.0, 100.0));

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Boxd kBoundsWithoutMiter = Boxd(Vector2d::Zero(), Vector2d(0.0, 100.0));

  // Simple bounds should not include miter.
  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);

  // Low cutoff, should not crash and be equal to bounds().
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);

  // More realistic miter values still work but always return value without miter.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 4.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), kBoundsWithoutMiter);
}

TEST(PathSpline, StrokeMiterBoundsInfinite) {
  // With a 180 degree angle, a line doubling back on itself.
  //
  //   .<===========>.
  // (0, 0)       (100, 0)
  //
  PathSpline spline;
  spline.moveTo(Vector2d::Zero());
  spline.lineTo(Vector2d(0.0, 100.0));
  spline.lineTo(Vector2d::Zero());

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Boxd kBoundsWithoutMiter = Boxd(Vector2d::Zero(), Vector2d(0.0, 100.0));

  // Simple bounds should not include miter.
  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);

  // Low cutoff, should not crash and be equal to bounds().
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);

  // More realistic miter values still work but always return value without miter.
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 4.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), kBoundsWithoutMiter);
}

TEST(PathSpline, PointAtTriangle) {
  //      (1, 2)
  //        /\
  //       /  \
  //      /    \
  //     /      \
  //    /        \
  //   /__________\
  //  (0, 0)      (2, 0)

  PathSpline spline;
  // Triangle.
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2(1.0, 2.0));
  spline.lineTo(Vector2(2.0, 0.0));
  spline.closePath();

  ASSERT_THAT(spline.commands(), SizeIs(4));

  // MoveTo should have the same point at the beginning and end.
  EXPECT_EQ(spline.commands()[0].type, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(0, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(0, 1.0), Vector2(0.0, 0.0));

  // First line: Lerps between start and end.
  EXPECT_EQ(spline.commands()[1].type, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(1, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 0.5), Vector2(0.5, 1.0));
  EXPECT_EQ(spline.pointAt(1, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[2].type, CommandType::LineTo);
  // This segment is just another line, so don't check it in detail.

  // ClosePath, which behaves like a line.
  EXPECT_EQ(spline.commands()[3].type, CommandType::ClosePath);
  EXPECT_EQ(spline.pointAt(3, 0.0), Vector2(2.0, 0.0));
  EXPECT_EQ(spline.pointAt(3, 0.5), Vector2(1.0, 0.0));
  EXPECT_EQ(spline.pointAt(3, 1.0), Vector2(0.0, 0.0));
}

TEST(PathSpline, PointAtMultipleSegments) {
  // Create two separate line segments.
  //
  //        . (1, 3)
  //        |
  //        |
  //        |
  //        ` (1, 1)
  //
  //   .__________.
  //  (0, 0)      (2, 0)

  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2(2.0, 0.0));

  spline.moveTo(Vector2(1.0, 1.0));
  spline.lineTo(Vector2(1.0, 3.0));

  ASSERT_THAT(spline.commands(), SizeIs(4));

  // MoveTo should have the same point at the beginning and end.
  EXPECT_EQ(spline.commands()[0].type, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(0, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(0, 1.0), Vector2(0.0, 0.0));

  // First line: Lerps between start and end.
  EXPECT_EQ(spline.commands()[1].type, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(1, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 0.5), Vector2(1.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 1.0), Vector2(2.0, 0.0));

  // Second MoveTo should have the same point at start/end.
  EXPECT_EQ(spline.commands()[2].type, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(2, 0.0), Vector2(1.0, 1.0));
  EXPECT_EQ(spline.pointAt(2, 1.0), Vector2(1.0, 1.0));

  // Second line: Lerps between start and end.
  EXPECT_EQ(spline.commands()[3].type, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(3, 0.0), Vector2(1.0, 1.0));
  EXPECT_EQ(spline.pointAt(3, 0.5), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.pointAt(3, 1.0), Vector2(1.0, 3.0));
}

TEST(PathSpline, TangentAt) {
  //     (1, 2)
  //       /\           .-"""-.
  //      /  \        /`       `\
  //     /    \      ;  (4, 1)   ;  r = 1
  //    /      \     ;     `     ;
  //   /        \     \         /
  //   ___________     `'-...-'`
  // (0, 0)      (1, 0)

  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2(1.0, 2.0));
  spline.lineTo(Vector2(2.0, 0.0));
  spline.closePath();

  spline.circle(Vector2d(4.0, 1.0), 1.0);

  ASSERT_THAT(spline.commands(), SizeIs(10));

  // Triangle.
  EXPECT_EQ(spline.commands()[0].type, CommandType::MoveTo);
  // MoveTo matches the next point.
  EXPECT_EQ(spline.tangentAt(0, 0.0), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(0, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[1].type, CommandType::LineTo);
  EXPECT_EQ(spline.tangentAt(1, 0.0), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(1, 0.5), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(1, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[2].type, CommandType::LineTo);
  EXPECT_EQ(spline.tangentAt(2, 0.0), Vector2(1.0, -2.0));
  EXPECT_EQ(spline.tangentAt(2, 1.0), Vector2(1.0, -2.0));

  EXPECT_EQ(spline.commands()[3].type, CommandType::ClosePath);
  EXPECT_EQ(spline.tangentAt(3, 0.0), Vector2(-2.0, 0.0));
  EXPECT_EQ(spline.tangentAt(3, 1.0), Vector2(-2.0, 0.0));

  // Circle.
  EXPECT_EQ(spline.commands()[4].type, CommandType::MoveTo);
  // MoveTo matches the next point.
  EXPECT_EQ(spline.pointAt(4, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.tangentAt(4, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.tangentAt(4, 1.0), Vector2Eq(0.0, Gt(0.0)));

  // Right side, going clockwise to bottom.
  EXPECT_EQ(spline.commands()[5].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(5, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.tangentAt(5, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.tangentAt(5, 0.5), NormalizedEq(Vector2(-1.0, 1.0)));
  EXPECT_THAT(spline.tangentAt(5, 1.0), Vector2Eq(Lt(0.0), 0.0));

  // Bottom, clockwise to left.
  EXPECT_EQ(spline.commands()[6].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(6, 0.0), Vector2(4.0, 2.0));
  EXPECT_THAT(spline.tangentAt(6, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.tangentAt(6, 0.5), NormalizedEq(Vector2(-1.0, -1.0)));
  EXPECT_THAT(spline.tangentAt(6, 1.0), Vector2Eq(0.0, Lt(0.0)));

  // Left, clockwise to top.
  EXPECT_EQ(spline.commands()[7].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(7, 0.0), Vector2(3.0, 1.0));
  EXPECT_THAT(spline.tangentAt(7, 0.0), Vector2Eq(0.0, Lt(0.0)));
  EXPECT_THAT(spline.tangentAt(7, 0.5), NormalizedEq(Vector2(1.0, -1.0)));
  EXPECT_THAT(spline.tangentAt(7, 1.0), Vector2Eq(Gt(0.0), 0.0));

  // Top, clockwise to right.
  EXPECT_EQ(spline.commands()[8].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(8, 0.0), Vector2(4.0, 0.0));
  EXPECT_THAT(spline.tangentAt(8, 0.0), Vector2Eq(Gt(0.0), 0.0));
  EXPECT_THAT(spline.tangentAt(8, 0.5), NormalizedEq(Vector2(1.0, 1.0)));
  EXPECT_THAT(spline.tangentAt(8, 1.0), Vector2Eq(0.0, Gt(0.0)));

  // Since there is no line segment, since the ClosePath is directly connected, the tangent is zero.
  EXPECT_EQ(spline.commands()[9].type, CommandType::ClosePath);
  EXPECT_EQ(spline.tangentAt(9, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.tangentAt(9, 1.0), Vector2(0.0, 0.0));
}

TEST(PathSpline, NormalAt) {
  //     (1, 2)
  //       /\           .-"""-.
  //      /  \        /`       `\
  //     /    \      ;  (4, 1)   ;  r = 1
  //    /      \     ;     `     ;
  //   /        \     \         /
  //   ___________     `'-...-'`
  // (0, 0)      (1, 0)

  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2(1.0, 2.0));
  spline.lineTo(Vector2(2.0, 0.0));
  spline.closePath();

  spline.circle(Vector2d(4.0, 1.0), 1.0);

  ASSERT_THAT(spline.commands(), SizeIs(10));

  // Triangle.
  EXPECT_EQ(spline.commands()[0].type, CommandType::MoveTo);
  // MoveTo matches the next point.
  EXPECT_EQ(spline.normalAt(0, 0.0), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(0, 1.0), Vector2(-2.0, 1.0));

  EXPECT_EQ(spline.commands()[1].type, CommandType::LineTo);
  EXPECT_EQ(spline.normalAt(1, 0.0), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(1, 0.5), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(1, 1.0), Vector2(-2.0, 1.0));

  EXPECT_EQ(spline.commands()[2].type, CommandType::LineTo);
  EXPECT_EQ(spline.normalAt(2, 0.0), Vector2(2.0, 1.0));
  EXPECT_EQ(spline.normalAt(2, 1.0), Vector2(2.0, 1.0));

  EXPECT_EQ(spline.commands()[3].type, CommandType::ClosePath);
  EXPECT_EQ(spline.normalAt(3, 0.0), Vector2(0.0, -2.0));
  EXPECT_EQ(spline.normalAt(3, 1.0), Vector2(0.0, -2.0));

  // Circle.
  EXPECT_EQ(spline.commands()[4].type, CommandType::MoveTo);
  // MoveTo matches the next point.
  EXPECT_EQ(spline.pointAt(4, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.normalAt(4, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(4, 1.0), Vector2Eq(Lt(0.0), 0.0));

  // Right side, going clockwise to bottom.
  EXPECT_EQ(spline.commands()[5].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(5, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.normalAt(5, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(5, 0.5), NormalizedEq(Vector2(-1.0, -1.0)));
  EXPECT_THAT(spline.normalAt(5, 1.0), Vector2Eq(0.0, Lt(0.0)));

  // Bottom, clockwise to left.
  EXPECT_EQ(spline.commands()[6].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(6, 0.0), Vector2(4.0, 2.0));
  EXPECT_THAT(spline.normalAt(6, 0.0), Vector2Eq(0.0, Lt(0.0)));
  EXPECT_THAT(spline.normalAt(6, 0.5), NormalizedEq(Vector2(1.0, -1.0)));
  EXPECT_THAT(spline.normalAt(6, 1.0), Vector2Eq(Gt(0.0), 0.0));

  // Left, clockwise to top.
  EXPECT_EQ(spline.commands()[7].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(7, 0.0), Vector2(3.0, 1.0));
  EXPECT_THAT(spline.normalAt(7, 0.0), Vector2Eq(Gt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(7, 0.5), NormalizedEq(Vector2(1.0, 1.0)));
  EXPECT_THAT(spline.normalAt(7, 1.0), Vector2Eq(0.0, Gt(0.0)));

  // Top, clockwise to right.
  EXPECT_EQ(spline.commands()[8].type, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(8, 0.0), Vector2(4.0, 0.0));
  EXPECT_THAT(spline.normalAt(8, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.normalAt(8, 0.5), NormalizedEq(Vector2(-1.0, 1.0)));
  EXPECT_THAT(spline.normalAt(8, 1.0), Vector2Eq(Lt(0.0), 0.0));

  // Since there is no line segment, since the ClosePath is directly connected, the normal is zero.
  EXPECT_EQ(spline.commands()[9].type, CommandType::ClosePath);
  EXPECT_EQ(spline.normalAt(9, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.normalAt(9, 1.0), Vector2(0.0, 0.0));
}

TEST(PathSpline, IsInsideSimpleTriangle) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2d(2.0, 0.0));
  spline.lineTo(Vector2d(1.0, 2.0));
  spline.closePath();

  // Point inside the triangle
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 1.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 1.0), FillRule::EvenOdd));

  // Point outside the triangle
  EXPECT_FALSE(spline.isInside(Vector2d(3.0, 1.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(3.0, 1.0), FillRule::EvenOdd));

  // Point on the edge of the triangle
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 0.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 0.0), FillRule::EvenOdd));
}

TEST(PathSpline, IsInsideComplexShape) {
  // Two squares, one inside the other
  //
  // (0, 4)                    (4, 4)
  // +------------<------------+
  // |  (1, 3)        (3, 3)   |
  // |    +-------<------+     |
  // |    |              |     |
  // v    v              ^     ^
  // |    |              |     |
  // |    +------->------+     |
  // |  (1, 1)        (3, 1)   |
  // +------------>------------+
  // (0, 0)                    (4, 0)

  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2d(4.0, 0.0));
  spline.lineTo(Vector2d(4.0, 4.0));
  spline.lineTo(Vector2d(0.0, 4.0));
  spline.closePath();
  spline.moveTo(Vector2d(1.0, 1.0));
  spline.lineTo(Vector2d(3.0, 1.0));
  spline.lineTo(Vector2d(3.0, 3.0));
  spline.lineTo(Vector2d(1.0, 3.0));
  spline.closePath();

  // Point inside the outer square but outside the inner square
  EXPECT_TRUE(spline.isInside(Vector2d(0.5, 0.5), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(0.5, 0.5), FillRule::EvenOdd));
  EXPECT_TRUE(spline.isInside(Vector2d(3.5, 2.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(3.5, 2.0), FillRule::EvenOdd));

  // Point inside the inner square
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 2.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(2.0, 2.0), FillRule::EvenOdd));

  // Point outside both squares
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 5.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 5.0), FillRule::EvenOdd));
}

TEST(PathSpline, IsInsideCurveShape) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.curveTo(Vector2d(1.0, 2.0), Vector2d(3.0, 2.0), Vector2d(4.0, 0.0));
  spline.curveTo(Vector2d(3.0, -2.0), Vector2d(1.0, -2.0), Vector2d(0.0, 0.0));
  spline.closePath();

  // Point inside the curve
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 0.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 0.0), FillRule::EvenOdd));

  // Point outside the curve
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 0.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 0.0), FillRule::EvenOdd));
}

TEST(PathSpline, IsInsideCircle) {
  PathSpline spline;
  spline.circle(Vector2d(0.0, 0.0), 5.0);

  // Point inside the circle
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 1.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(1.0, 1.0), FillRule::EvenOdd));

  // Point on the circle boundary
  EXPECT_TRUE(spline.isInside(Vector2d(5.0, 0.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(5.0, 0.0), FillRule::EvenOdd));

  // Point outside the circle
  EXPECT_FALSE(spline.isInside(Vector2d(6.0, 0.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(6.0, 0.0), FillRule::EvenOdd));
}

TEST(PathSpline, IsInsideMultipleSubpaths) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2d(4.0, 0.0));
  spline.lineTo(Vector2d(4.0, 4.0));
  spline.lineTo(Vector2d(0.0, 4.0));
  spline.closePath();
  spline.moveTo(Vector2d(5.0, 5.0));
  spline.lineTo(Vector2d(7.0, 5.0));
  spline.lineTo(Vector2d(7.0, 7.0));
  spline.lineTo(Vector2d(5.0, 7.0));
  spline.closePath();

  // Point inside the first subpath
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 2.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 2.0), FillRule::EvenOdd));

  // Point inside the second subpath
  EXPECT_TRUE(spline.isInside(Vector2d(6.0, 6.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(6.0, 6.0), FillRule::EvenOdd));

  // Point outside both subpaths
  EXPECT_FALSE(spline.isInside(Vector2d(8.0, 8.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(8.0, 8.0), FillRule::EvenOdd));
}

// Test for the 'appendJoin' method
TEST(PathSpline, AppendJoin) {
  PathSpline spline1;
  spline1.moveTo(kVec1);
  spline1.lineTo(kVec2);

  PathSpline spline2;
  spline2.moveTo(kVec2);
  spline2.lineTo(kVec3);

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}));
}

// appendJoin should remove the first moveTo, so if the start/stop points don't match there is a
// change
TEST(PathSpline, AppendJoinWithJump) {
  PathSpline spline1;
  spline1.moveTo(kVec1);
  spline1.lineTo(kVec2);

  PathSpline spline2;
  spline2.moveTo(kVec3);
  spline2.lineTo(kVec4);

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec4));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}));
}

}  // namespace donner::svg
