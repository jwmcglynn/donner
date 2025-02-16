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

TEST(PathSpline, CommandTypeOstreamOutput) {
  EXPECT_THAT(CommandType::MoveTo, ToStringIs("MoveTo"));
  EXPECT_THAT(CommandType::LineTo, ToStringIs("LineTo"));
  EXPECT_THAT(CommandType::CurveTo, ToStringIs("CurveTo"));
  EXPECT_THAT(CommandType::ClosePath, ToStringIs("ClosePath"));
}

TEST(PathSpline, CommandOstreamOutput) {
  EXPECT_THAT(Command(CommandType::MoveTo, 0), ToStringIs("Command {MoveTo, 0}"));
  EXPECT_THAT(Command(CommandType::LineTo, 1), ToStringIs("Command {LineTo, 1}"));
  EXPECT_THAT(Command(CommandType::CurveTo, 2), ToStringIs("Command {CurveTo, 2}"));
  EXPECT_THAT(Command(CommandType::ClosePath, 3), ToStringIs("Command {ClosePath, 3}"));
}

TEST(PathSpline, DefaultConstruction) {
  PathSpline spline;
  EXPECT_TRUE(spline.empty());
  EXPECT_THAT(spline.points(), SizeIs(0));
  EXPECT_THAT(spline.commands(), SizeIs(0));
}

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

/// @test that after closing a path, a subsequent lineTo auto‑reopens the subpath.
TEST(PathSpline, AutoReopenOnLineTo) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(10, 0));
  spline.closePath();
  // Without an explicit moveTo, lineTo should trigger an auto-reopen.
  spline.lineTo(Vector2d(20, 0));

  // Expected sequence:
  //  - MoveTo (index 0, point (0,0))
  //  - LineTo (index 1, point (10,0))
  //  - ClosePath (using the original moveTo index)
  //  - Auto-inserted MoveTo (with same point as original moveTo)
  //  - LineTo (new point (20,0))
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command(CommandType::MoveTo, 0), Command(CommandType::LineTo, 1),
                          Command(CommandType::ClosePath, 0), Command(CommandType::MoveTo, 0),
                          Command(CommandType::LineTo, 2)));

  // The points vector should not duplicate the auto-reopened moveTo point.
  EXPECT_THAT(
      spline.points(),
      ElementsAre(Vector2d(0, 0), Vector2d(10, 0),  // From the initial segment
                  Vector2d(20, 0)  // The next point should be the lineTo, not the auto moveTo.
                  ));
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

/// @test simple usage of arcTo.
TEST(PathSpline, ArcTo) {
  PathSpline spline;
  spline.moveTo(Vector2d(1.0, 0.0));
  spline.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
               Vector2d(0.0, 2.0));

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

/// @test arcTo with a the large arc flag, validating that it sweeps the larger arc.
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

/// @test that calling arcTo with identical start and end points does nothing.
TEST(PathSpline, ArcToDegenerate) {
  PathSpline spline;
  Vector2d pt(1, 1);
  spline.moveTo(pt);
  // With start == end the arc should not be decomposed into curves.
  spline.arcTo(Vector2d(10, 10), 0.0, false, false, pt);
  EXPECT_THAT(spline.points(), ElementsAre(pt));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(CommandType::MoveTo, 0)));
}

TEST(PathSpline, ArcToZeroRadius) {
  // When the radius is zero, arcTo should fall back to a line segment.
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.arcTo(Vector2d(0, 0), 0.0, false, false, Vector2d(10, 0));

  // Expect a simple line: one MoveTo and one LineTo command.
  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(0, 0), Vector2d(10, 0)));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(PathSpline::CommandType::MoveTo, 0),
                                             Command(PathSpline::CommandType::LineTo, 1)));
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

TEST(PathSpline, ClosePathNeedsMoveToReopen) {
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

TEST(PathSpline, PathLengthClosedPath) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(1, 0));
  spline.lineTo(Vector2d(1, 1));
  spline.lineTo(Vector2d(0, 1));
  spline.closePath();

  // Calculate the expected length of the closed path
  const double expectedLength = 4.0;
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(PathSpline, PathLengthSubdivideExceedsMaxRecursion) {
  PathSpline spline;
  // Start at (0,0)
  spline.moveTo(Vector2d(0.0, 0.0));
  // Create an extremely “curvy” cubic Bezier curve:
  //   p0 = (0,0)
  //   p1 = (0,10000)  — a huge jump upward
  //   p2 = (0,-10000) — a huge jump downward
  //   p3 = (1,0)
  //
  // The chord from p0 to p3 is length 1.0. However, the control polygon has
  // a very large length. This forces the recursion to never satisfy the flatness
  // criterion, so eventually the function will hit the branch that returns the
  // chord length.
  spline.curveTo(Vector2d(0.0, 10000.0), Vector2d(0.0, -10000.0), Vector2d(1.0, 0.0));

  // Because the maximum recursion depth is exceeded, a slightly inprecise value is returned.
  EXPECT_NEAR(spline.pathLength(), 11547.003595164915, 1e-6);
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

TEST(PathSpline, TransformedBoundsIdentity) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2d(1.0, 0.0));
  spline.lineTo(Vector2d(1.0, 1.0));
  spline.lineTo(Vector2d(0.0, 1.0));
  spline.closePath();

  const Transformd identityTransform = Transformd();
  EXPECT_EQ(spline.transformedBounds(identityTransform), spline.bounds());
}

TEST(PathSpline, TransformedBoundsTranslation) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.lineTo(Vector2d(2.0, 0.0));
  spline.lineTo(Vector2d(2.0, 2.0));
  spline.lineTo(Vector2d(0.0, 2.0));
  spline.closePath();

  const Transformd translationTransform = Transformd::Translate(3.0, 4.0);
  const Boxd expectedBounds(Vector2d(3.0, 4.0), Vector2d(5.0, 6.0));

  EXPECT_EQ(spline.transformedBounds(translationTransform), expectedBounds);
}

TEST(PathSpline, TransformedBoundsRotation) {
  PathSpline spline;
  spline.moveTo(Vector2d(1.0, 1.0));
  spline.lineTo(Vector2d(3.0, 1.0));
  spline.lineTo(Vector2d(3.0, 3.0));
  spline.lineTo(Vector2d(1.0, 3.0));
  spline.closePath();

  const Transformd rotationTransform = Transformd::Rotate(MathConstants<double>::kPi / 4);
  const Boxd transformedBounds = spline.transformedBounds(rotationTransform);

  // Expected bounds after rotation
  const double sqrt2 = std::sqrt(2.0);
  const Boxd expectedBounds(Vector2d(-sqrt2, sqrt2), Vector2d(sqrt2, 3 * sqrt2));

  EXPECT_THAT(transformedBounds.topLeft, Vector2Near(-sqrt2, sqrt2));
  EXPECT_THAT(transformedBounds.bottomRight, Vector2Near(sqrt2, 3 * sqrt2));
}

TEST(PathSpline, TransformedBoundsScaling) {
  PathSpline spline;
  spline.moveTo(Vector2d(-1.0, -1.0));
  spline.lineTo(Vector2d(1.0, -1.0));
  spline.lineTo(Vector2d(1.0, 1.0));
  spline.lineTo(Vector2d(-1.0, 1.0));
  spline.closePath();

  const Transformd scalingTransform = Transformd::Scale(2.0);
  const Boxd expectedBounds(Vector2d(-2.0, -2.0), Vector2d(2.0, 2.0));

  EXPECT_EQ(spline.transformedBounds(scalingTransform), expectedBounds);
}

TEST(PathSpline, TransformedBoundsComplexTransform) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.curveTo(Vector2d(1.0, 2.0), Vector2d(3.0, 2.0), Vector2d(4.0, 0.0));

  const Transformd complexTransform =
      Transformd::Scale(0.5) *
      Transformd::Rotate(MathConstants<double>::kHalfPi)  // Rotate by 90 degrees
      * Transformd::Translate(2.0, -1.0);
  const Boxd transformedBounds = spline.transformedBounds(complexTransform);

  EXPECT_THAT(transformedBounds.topLeft, Vector2Near(1.25, -1));
  EXPECT_THAT(transformedBounds.bottomRight, Vector2Near(2, 1));
}

TEST(PathSpline, TransformedBoundsEmptySpline) {
  PathSpline spline;
  const Transformd anyTransform = Transformd();
  EXPECT_DEATH(spline.transformedBounds(anyTransform), "!empty()");
}

/**
 * @test that the bounds of a path with a degenerate x-extrema are correctly transformed.
 */
TEST(PathSpline, TransformedBoundsDegenerateXExtrema) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  // A cubic curve with a degenerate x-extrema at t=0.5.
  // Using:
  //   start = (0,0)
  //   controlPoint1 = (1,1)
  //   controlPoint2 = (1,2)
  //   end = (0,0)
  // yields:
  //   a.x = 3 * (-0 + 3*1 - 3*1 + 0) = 0,
  //   b.x = 6 * (0 + 1 - 2*1) = -6 (non-zero),
  //   c.x = 3 * ( -0 + 1 ) = 3, so t = -c.x/b.x = 0.5.
  spline.curveTo(Vector2d(1.0, 0.0), Vector2d(1.0, 0.0), Vector2d(0.0, 0.0));

  // In the original coordinate space the curve reaches a maximum point at t=0.5
  EXPECT_THAT(spline.pointAt(1, 0.5), Vector2Near(0.75, 0.0));

  // Apply a 90-degree rotation about the origin.
  const Transformd rotation90 = Transformd::Rotate(MathConstants<double>::kHalfPi);
  const Boxd bounds = spline.transformedBounds(rotation90);
  EXPECT_THAT(bounds, BoxEq(Vector2Near(0.0, 0.0), Vector2d(0.0, 0.75)));
}

/**
 * @test that the bounds of a path with a degenerate y-extrema are correctly transformed.
 */
TEST(PathSpline, TransformedBoundsDegenerateYExtrema) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  // Create a cubic curve with a degenerate y-extrema.
  // Using:
  //   start = (0,0)
  //   controlPoint1 = (1,1)
  //   controlPoint2 = (2,1)
  //   end = (0,0)
  // yields:
  //   a.y = 3 * (-0 + 3*1 - 3*1 + 0) = 0,
  //   b.y = 6 * (0 + 1 - 2*1) = -6 (non-zero),
  //   c.y = 3 * ( -0 + 1 ) = 3, so t = -c.y/b.y = 0.5.
  spline.curveTo(Vector2d(0.0, 1.0), Vector2d(0.0, 1.0), Vector2d(0.0, 0.0));

  // In the original coordinate space the curve reaches a maximum point at t=0.5
  EXPECT_THAT(spline.pointAt(1, 0.5), Vector2Near(0.0, 0.75));

  // Apply a 90-degree rotation about the origin (rotation: (x,y) -> (-y,x)).
  const Transformd rotation90 = Transformd::Rotate(MathConstants<double>::kHalfPi);
  const Boxd bounds = spline.transformedBounds(rotation90);

  EXPECT_THAT(bounds, BoxEq(Vector2d(-0.75, 0.0), Vector2Near(0.0, 0.0)));
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

/// @test that a degenerate cubic curve (with control points equal to the start)
/// triggers the branch that adjusts the t value when the derivative is near zero.
TEST(PathSpline, TangentAtDegenerateCurve) {
  PathSpline spline;
  const Vector2d start(0, 0), degenerate(0, 0), end(1, 0);
  spline.moveTo(start);
  spline.curveTo(degenerate, degenerate, end);

  // Command index 1 is the CurveTo.
  // For t = 0, the derivative is zero so the code adjusts to t = 0.01.
  const Vector2d tangent0 = spline.tangentAt(1, 0.0);
  const Vector2d tangentAdjusted = spline.tangentAt(1, 0.01);
  EXPECT_THAT(tangent0.x, DoubleNear(tangentAdjusted.x, 1e-6));
  EXPECT_THAT(tangent0.y, DoubleNear(tangentAdjusted.y, 1e-6));

  // For this degenerate curve, the expected derivative at t=0.01 is approximately
  // 3 * (0.01^2) * (end - degenerate) = (0.0003, 0).
  EXPECT_NEAR(tangent0.x, 0.0003, 1e-6);
  EXPECT_NEAR(tangent0.y, 0.0, 1e-6);
}

/// @test that calling tangentAt on a spline with only a MoveTo returns zero.
TEST(PathSpline, TangentAtSingleMoveTo) {
  PathSpline spline;
  spline.moveTo(Vector2d(5, 5));
  EXPECT_EQ(spline.tangentAt(0, 0.0), Vector2d::Zero());
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

/// @test appendJoin with two simple paths, one ending at the start of the other.
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

/// @test that appendJoin removes the first moveTo, so if the start/stop points don't match the path
/// is still continuous.
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

/// @test that appending an empty spline leaves the original spline unchanged.
TEST(PathSpline, AppendJoinEmpty) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);

  PathSpline emptySpline;
  spline.appendJoin(emptySpline);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command(CommandType::MoveTo, 0), Command(CommandType::LineTo, 1)));
}

/// @test appendJoin with a second path that has multiple MoveTo commands.
TEST(PathSpline, AppendJoinWithMultipleMoveTo) {
  PathSpline spline1;
  spline1.moveTo(kVec1);
  spline1.lineTo(kVec2);

  PathSpline spline2;
  spline2.moveTo(kVec2);  // Should match end of spline1
  spline2.lineTo(kVec3);
  spline2.moveTo(kVec4);  // Second MoveTo creates new subpath
  spline2.lineTo(kVec1);

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}, Command{CommandType::MoveTo, 3},
                          Command{CommandType::LineTo, 4}));
}

TEST(PathSpline, VerticesSimple) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.lineTo(kVec3);
  spline.lineTo(kVec4);

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec4));
}

TEST(PathSpline, VerticesOstreamOutput) {
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(3.0, 4.0));

  EXPECT_THAT(spline.vertices(), ToStringIs("{ Vertex(point=(0, 0), orientation=(0.6, 0.8)), "
                                            "Vertex(point=(3, 4), orientation=(0.6, 0.8)) }"));
}

TEST(PathSpline, VerticesWithJump) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.moveTo(kVec3);
  spline.lineTo(kVec4);

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec4));
}

TEST(PathSpline, VerticesClosePath) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.lineTo(kVec3);
  spline.closePath();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec1));
}

TEST(PathSpline, VerticesClosePathWithoutLine) {
  PathSpline spline;
  spline.moveTo(kVec1);
  spline.lineTo(kVec2);
  spline.moveTo(kVec1);
  spline.closePath();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec1));
}

TEST(PathSpline, VerticesCircle) {
  PathSpline spline;
  spline.circle(Vector2d(0.0, 0.0), 5.0);
  EXPECT_THAT(spline.vertices(),
              VertexPointsAre(Vector2d(5.0, 0.0), Vector2d(0.0, 5.0), Vector2d(-5.0, 0.0),
                              Vector2d(0.0, -5.0), Vector2d(5.0, 0.0)));
}

TEST(PathSpline, VerticesArc) {
  PathSpline spline;
  spline.moveTo(Vector2d(0.0, 0.0));
  spline.arcTo(Vector2d(5.0, 5.0), 0.0, /*largeArcFlag=*/true, /*sweepFlag=*/true,
               Vector2d(5.0, 0.0));

  EXPECT_THAT(spline.vertices(), VertexPointsAre(Vector2d(0.0, 0.0), Vector2Near(5.0, 0.0)));
}

/// @test that isOnPath works for a simple line segment.
TEST(PathSpline, IsOnPathLine) {
  // Create a simple horizontal line from (0,0) to (10,0)
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(10, 0));

  // Exactly on the line.
  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0), 0.001));

  // Within stroke tolerance.
  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0.05), 0.1));

  // Outside the stroke tolerance.
  EXPECT_FALSE(spline.isOnPath(Vector2d(5, 0.2), 0.1));
}

/// @test that isOnPath works for a cubic Bezier curve.
TEST(PathSpline, IsOnPathCurve) {
  // Create a cubic Bezier curve:
  // p0 = (0,0), p1 = (5,0), p2 = (5,10), p3 = (0,10)
  // The midpoint of this curve can be computed (for t=0.5) as:
  // B(0.5) = (3.75, 5.0)
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.curveTo(Vector2d(5, 0), Vector2d(5, 10), Vector2d(0, 10));

  // Test a point exactly on the curve (at t=0.5).
  EXPECT_TRUE(spline.isOnPath(Vector2d(3.75, 5.0), 0.1));

  // Test a point that is near—but not on—the curve.
  EXPECT_FALSE(spline.isOnPath(Vector2d(3.9, 5.0), 0.1));
}

/// @test isOnPath for multiple line segments.
TEST(PathSpline, IsOnPathMultiSegment) {
  // Create a triangle:
  //   (0,0) -> (5,0) -> (2.5,5) -> back to (0,0)
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(5, 0));
  spline.lineTo(Vector2d(2.5, 5));
  spline.closePath();

  // Test a point on the base edge.
  EXPECT_TRUE(spline.isOnPath(Vector2d(2.5, 0), 0.001));

  // Test a point on the edge from (5,0) to (2.5,5) – linear interpolation yields (3.75,2.5) at
  // t=0.5.
  EXPECT_TRUE(spline.isOnPath(Vector2d(3.75, 2.5), 0.001));

  // Test a point on the closePath edge.
  // Test a point on the edge from (2.5,5) back to (0,0) – linear interpolation yields (1.25,2.5) at
  // t=0.5.
  EXPECT_TRUE(spline.isOnPath(Vector2d(1.25, 2.5), 0.001));

  // A point not on any edge.
  EXPECT_FALSE(spline.isOnPath(Vector2d(2.5, 2), 0.001));
}

/// @test isOnPath if the path only has a moveTo command.
TEST(PathSpline, IsOnPathMoveToOnly) {
  // When the path contains only a MoveTo, there is no segment to be on.
  PathSpline spline;
  spline.moveTo(Vector2d(1, 1));
  EXPECT_FALSE(spline.isOnPath(Vector2d(1, 1), 0.1));
}

/// @test isOnPath with no stroke width.
TEST(PathSpline, IsOnPathZeroStrokeWidth) {
  // With a strokeWidth of zero, only a point exactly on the segment qualifies.
  PathSpline spline;
  spline.moveTo(Vector2d(0, 0));
  spline.lineTo(Vector2d(10, 0));

  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0), 0.0));
  // Even a very small deviation should fail.
  EXPECT_FALSE(spline.isOnPath(Vector2d(5, 0.0001), 0.0));
}

}  // namespace donner::svg
