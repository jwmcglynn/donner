#include "donner/base/Path.h"

#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/tests/PathTestUtils.h"

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

using Command = Path::Command;
using CommandType = Path::Verb;

constexpr Vector2d kVec1(123.0, 456.7);
constexpr Vector2d kVec2(78.9, 1011.12);
constexpr Vector2d kVec3(-1314.0, 1516.17);
constexpr Vector2d kVec4(1819.0, -2021.22);

}  // namespace

TEST(Path, CommandTypeOstreamOutput) {
  EXPECT_THAT(CommandType::MoveTo, ToStringIs("MoveTo"));
  EXPECT_THAT(CommandType::LineTo, ToStringIs("LineTo"));
  EXPECT_THAT(CommandType::CurveTo, ToStringIs("CurveTo"));
  EXPECT_THAT(CommandType::ClosePath, ToStringIs("ClosePath"));
}

TEST(Path, CommandOstreamOutput) {
  EXPECT_THAT(Command(CommandType::MoveTo, 0), ToStringIs("Command {MoveTo, 0}"));
  EXPECT_THAT(Command(CommandType::LineTo, 1), ToStringIs("Command {LineTo, 1}"));
  EXPECT_THAT(Command(CommandType::CurveTo, 2), ToStringIs("Command {CurveTo, 2}"));
  EXPECT_THAT(Command(CommandType::ClosePath, 3), ToStringIs("Command {ClosePath, 3}"));
}

TEST(Path, DefaultConstruction) {
  Path spline;
  EXPECT_TRUE(spline.empty());
  EXPECT_THAT(spline.points(), SizeIs(0));
  EXPECT_THAT(spline.commands(), SizeIs(0));
}

TEST(Path, MoveTo) {
  Path spline = PathBuilder().moveTo(kVec1).build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(CommandType::MoveTo, 0)));
}

TEST(Path, MoveToMultiple) {
  Path spline = PathBuilder().moveTo(kVec1).moveTo(kVec2).build();

  // PathBuilder does not collapse consecutive moveTos; both are emitted.
  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command(CommandType::MoveTo, 0), Command(CommandType::MoveTo, 1)));
}

TEST(Path, MoveToMultipleSegments) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .moveTo(kVec3)
                    .lineTo(kVec4)
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3}));
}

TEST(Path, MoveToUnused) {
  Path spline = PathBuilder().moveTo(kVec1).lineTo(kVec2).moveTo(kVec3).build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}));
}

/// @test that after closing a path, a subsequent lineTo auto‑reopens the subpath.
TEST(Path, AutoReopenOnLineTo) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .lineTo(Vector2d(10, 0))
                    .closePath()
                    // Without an explicit moveTo, lineTo should trigger an auto-reopen.
                    .lineTo(Vector2d(20, 0))
                    .build();

  // Expected sequence:
  //  - MoveTo (index 0, point (0,0))
  //  - LineTo (index 1, point (10,0))
  //  - ClosePath (pointIndex = 0, the moveToPointIndex)
  //  - Auto-inserted MoveTo (re-opens at (0,0))
  //  - LineTo (new point (20,0))
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command(CommandType::MoveTo, 0), Command(CommandType::LineTo, 1),
                          Command(CommandType::ClosePath, 0), Command(CommandType::MoveTo, 2),
                          Command(CommandType::LineTo, 3)));

  // The auto-reopened moveTo duplicates the point (0,0) because PathBuilder always pushes it.
  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(0, 0), Vector2d(10, 0), Vector2d(0, 0), Vector2d(20, 0)));
}

TEST(Path, LineTo) {
  Path spline = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
}

TEST(Path, LineToComplex) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d::Zero())
                    .lineTo(kVec1)
                    // Create a separate line with two segments.
                    .moveTo(Vector2d::Zero())
                    .lineTo(kVec2)
                    .lineTo(kVec1)
                    .build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d::Zero(), kVec1, Vector2d::Zero(), kVec2, kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3},
                          Command{CommandType::LineTo, 4}));
}

// TODO(geode): PathBuilder auto-inserts moveTo via ensureMoveTo(), so this death test no longer
// applies. If Path validation is added later, re-enable this test.
// TEST(Path, LineToFailsWithoutStart) {
//   PathBuilder builder;
//   EXPECT_DEATH(builder.lineTo(kVec1), "without calling moveTo");
// }

TEST(Path, CurveTo) {
  Path spline = PathBuilder().moveTo(kVec1).curveTo(kVec2, kVec3, kVec4).build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(Path, CurveToChained) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .curveTo(kVec2, kVec3, kVec4)
                    .curveTo(kVec1, kVec2, Vector2d::Zero())
                    .lineTo(kVec1)
                    .build();

  EXPECT_THAT(spline.points(),
              ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1, kVec2, Vector2d::Zero(), kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::LineTo, 7}));
}

// TODO(geode): PathBuilder auto-inserts moveTo via ensureMoveTo(), so this death test no longer
// applies. If Path validation is added later, re-enable this test.
// TEST(Path, CurveToFailsWithoutStart) {
//   PathBuilder builder;
//   EXPECT_DEATH(builder.curveTo(kVec1, kVec2, kVec3), "without calling moveTo");
// }

/// @test simple usage of arcTo.
TEST(Path, ArcTo) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(1.0, 0.0))
                    .arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
                           Vector2d(0.0, 2.0))
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

/// @test arcTo with a the large arc flag, validating that it sweeps the larger arc.
TEST(Path, ArcToLargeArc) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(1.0, 0.0))
                    .arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, true, false,
                           Vector2d(0.0, 2.0))
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, -2.0), _, _, _,
                                           _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7}));
}

/// @test that calling arcTo with identical start and end points does nothing.
TEST(Path, ArcToDegenerate) {
  Vector2d pt(1, 1);
  Path spline = PathBuilder()
                    .moveTo(pt)
                    .arcTo(Vector2d(10, 10), 0.0, false, false, pt)
                    .build();
  EXPECT_THAT(spline.points(), ElementsAre(pt));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(CommandType::MoveTo, 0)));
}

TEST(Path, ArcToZeroRadius) {
  // When the radius is zero, arcTo should fall back to a line segment.
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .arcTo(Vector2d(0, 0), 0.0, false, false, Vector2d(10, 0))
                    .build();

  // Expect a simple line: one MoveTo and one LineTo command.
  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(0, 0), Vector2d(10, 0)));
  EXPECT_THAT(spline.commands(), ElementsAre(Command(Path::Verb::MoveTo, 0),
                                             Command(Path::Verb::LineTo, 1)));
}

TEST(Path, ClosePath) {
  Path spline = PathBuilder().moveTo(kVec1).lineTo(kVec2).closePath().build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}));
}

TEST(Path, ClosePathNeedsMoveToReopen) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .closePath()
                    .moveTo(kVec1)
                    .lineTo(kVec3)
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec1, kVec3));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                          Command{CommandType::LineTo, 3}));
}

// TODO(geode): PathBuilder::closePath() does not validate whether a moveTo has been issued.
// If Path validation is added later, re-enable this test.
// TEST(Path, ClosePathFailsWithoutStart) {
//   PathBuilder builder;
//   EXPECT_DEATH(builder.closePath(), "without an open path");
// }

TEST(Path, ClosePathAfterMoveTo) {
  Path spline = PathBuilder().moveTo(kVec1).closePath().build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
}

TEST(Path, ClosePathMoveToReplace) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .closePath()
                    .moveTo(kVec3)
                    .lineTo(kVec4)
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                          Command{CommandType::LineTo, 3}));
}

TEST(Path, ConsecutiveClosePathIsNoOp) {
  // Second closePath is a no-op since no subpath is open.
  Path spline =
      PathBuilder().moveTo(kVec1).lineTo(kVec2).closePath().closePath().build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0}));
}

TEST(Path, ConsecutiveClosePathThenNewSubpath) {
  // Second closePath is a no-op, then new subpath opens normally.
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .closePath()
                    .closePath()
                    .moveTo(kVec3)
                    .lineTo(kVec4)
                    .build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::ClosePath, 0},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3}));
}

// Regression test: many consecutive closePath calls after a single subpath.
// Extra closePaths are no-ops and should not corrupt state.
TEST(Path, ManyConsecutiveClosePathsDoNotCorrupt) {
  PathBuilder builder;
  builder.moveTo(kVec1);
  builder.lineTo(kVec2);
  builder.closePath();

  // Hammer closePath 100 times — all are no-ops (no open subpath).
  for (int i = 0; i < 100; ++i) {
    builder.closePath();
  }

  // Open a new subpath — should work normally.
  builder.moveTo(kVec3);
  builder.lineTo(kVec4);
  builder.closePath();

  Path spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  // 3 initial commands + 3 final commands = 6 (100 no-ops produced nothing).
  EXPECT_EQ(spline.verbCount(), 6u);
}

TEST(Path, Ellipse) {
  const Vector2d center(0.0, 1.0);
  const Vector2d radius(2.0, 1.0);
  Path spline =
      PathBuilder().addEllipse(Box2d(center - radius, center + radius)).build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 2.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 0.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::ClosePath, 0}));
}

TEST(Path, Circle) {
  Path spline = PathBuilder().addCircle(Vector2d(0.0, 1.0), 2.0).build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 3.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, -1.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::ClosePath, 0}));
}

TEST(Path, Empty) {
  Path spline;
  EXPECT_TRUE(spline.empty());
}

TEST(Path, PathLengthEmpty) {
  Path spline;
  EXPECT_EQ(spline.pathLength(), 0.0);
}

TEST(Path, PathLengthSingleLine) {
  Path spline = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();

  const double expectedLength = (kVec2 - kVec1).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(Path, PathLengthMultipleSegments) {
  Path spline =
      PathBuilder().moveTo(kVec1).lineTo(kVec2).lineTo(kVec3).lineTo(kVec4).build();

  const double expectedLength =
      (kVec2 - kVec1).length() + (kVec3 - kVec2).length() + (kVec4 - kVec3).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(Path, PathLengthCurveTo) {
  Path spline = PathBuilder().moveTo(kVec1).curveTo(kVec2, kVec3, kVec4).build();

  const double tolerance = 0.001;
  double expectedLength = 4106.97786;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthComplexPath) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .curveTo(kVec3, kVec4, Vector2d(1.0, 1.0))
                    .arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
                           Vector2d(0.0, 2.0))
                    .build();

  SCOPED_TRACE(testing::Message() << "Path: " << spline);

  // Value is saved from a previous run, it should not change.
  const double tolerance = 0.001;
  double expectedLength = 3674.25092;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthSimpleCurve) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(1, 2), Vector2d(3, 2), Vector2d(4, 0))
                    .build();

  // Calculate the expected length of the simple cubic Bezier curve
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 5.26836554;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthLoop) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(1, 2), Vector2d(3, -2), Vector2d(4, 0))
                    .build();

  // Calculate the expected length of the cubic Bezier curve with a loop
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 4.79396527;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthCusp) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(1, 2), Vector2d(2, 2), Vector2d(3, 0))
                    .build();

  // Calculate the expected length of the cubic Bezier curve with a cusp
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 4.43682857;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthInflectionPoint) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(1, 2), Vector2d(2, -2), Vector2d(3, 0))
                    .build();

  // Calculate the expected length of the cubic Bezier curve with an inflection point
  // Values from: bazel run //donner/svg/core:generate_test_pathlength_numpy
  const double expectedLength = 3.93406628;
  const double tolerance = 0.001;
  EXPECT_NEAR(spline.pathLength(), expectedLength, tolerance);
}

TEST(Path, PathLengthCollinearControlPoints) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(1, 1), Vector2d(2, 2), Vector2d(3, 3))
                    .build();

  // For collinear control points, the curve should be a straight line
  const double expectedLength = (Vector2d(3, 3) - Vector2d(0, 0)).length();
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(Path, PathLengthClosedPath) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .lineTo(Vector2d(1, 0))
                    .lineTo(Vector2d(1, 1))
                    .lineTo(Vector2d(0, 1))
                    .closePath()
                    .build();

  // Calculate the expected length of the closed path
  const double expectedLength = 4.0;
  EXPECT_DOUBLE_EQ(spline.pathLength(), expectedLength);
}

TEST(Path, PathLengthSubdivideExceedsMaxRecursion) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    // Create an extremely “curvy” cubic Bezier curve:
                    //   p0 = (0,0)
                    //   p1 = (0,10000)  -- a huge jump upward
                    //   p2 = (0,-10000) -- a huge jump downward
                    //   p3 = (1,0)
                    //
                    // The chord from p0 to p3 is length 1.0. However, the control polygon has
                    // a very large length. This forces the recursion to never satisfy the flatness
                    // criterion, so eventually the function will hit the branch that returns the
                    // chord length.
                    .curveTo(Vector2d(0.0, 10000.0), Vector2d(0.0, -10000.0), Vector2d(1.0, 0.0))
                    .build();

  // Because the maximum recursion depth is exceeded, a slightly inprecise value is returned.
  EXPECT_NEAR(spline.pathLength(), 11547.003595164915, 1e-6);
}

TEST(Path, BoundsEmpty) {
  Path spline;
  // Path::bounds() returns a default Box2d for an empty path.
  EXPECT_EQ(spline.bounds(), Box2d());
}

TEST(Path, Bounds) {
  Path spline =
      PathBuilder().moveTo(Vector2d::Zero()).lineTo(kVec1).lineTo(kVec2).build();

  EXPECT_EQ(spline.bounds(), Box2d(Vector2d(0.0, 0.0), Vector2d(123.0, 1011.12)));
}

TEST(Path, BoundsCurve) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .curveTo(Vector2d(8.0, 9.0), Vector2d(2.0, 0.0), Vector2d(0.0, 0.0))
                    .build();

  EXPECT_THAT(spline.bounds(), BoxEq(Vector2d(0.0, 0.0), Vector2Near(4.04307, 4.0)));
}

TEST(Path, BoundsEllipse) {
  const Vector2d center(1.0, 2.0);
  const Vector2d radius(2.0, 1.0);
  Path spline =
      PathBuilder().addEllipse(Box2d(center - radius, center + radius)).build();

  EXPECT_THAT(spline.bounds(), Box2d(Vector2d(-1.0, 1.0), Vector2d(3.0, 3.0)));
}

TEST(Path, TransformedBoundsIdentity) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2d(1.0, 0.0))
                    .lineTo(Vector2d(1.0, 1.0))
                    .lineTo(Vector2d(0.0, 1.0))
                    .closePath()
                    .build();

  const Transform2d identityTransform = Transform2d();
  EXPECT_EQ(spline.transformedBounds(identityTransform), spline.bounds());
}

TEST(Path, TransformedBoundsTranslation) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2d(2.0, 0.0))
                    .lineTo(Vector2d(2.0, 2.0))
                    .lineTo(Vector2d(0.0, 2.0))
                    .closePath()
                    .build();

  const Transform2d translationTransform = Transform2d::Translate(3.0, 4.0);
  const Box2d expectedBounds(Vector2d(3.0, 4.0), Vector2d(5.0, 6.0));

  EXPECT_EQ(spline.transformedBounds(translationTransform), expectedBounds);
}

TEST(Path, TransformedBoundsRotation) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(1.0, 1.0))
                    .lineTo(Vector2d(3.0, 1.0))
                    .lineTo(Vector2d(3.0, 3.0))
                    .lineTo(Vector2d(1.0, 3.0))
                    .closePath()
                    .build();

  const Transform2d rotationTransform = Transform2d::Rotate(MathConstants<double>::kPi / 4);
  const Box2d transformedBounds = spline.transformedBounds(rotationTransform);

  // Expected bounds after rotation
  const double sqrt2 = std::sqrt(2.0);
  const Box2d expectedBounds(Vector2d(-sqrt2, sqrt2), Vector2d(sqrt2, 3 * sqrt2));

  EXPECT_THAT(transformedBounds.topLeft, Vector2Near(-sqrt2, sqrt2));
  EXPECT_THAT(transformedBounds.bottomRight, Vector2Near(sqrt2, 3 * sqrt2));
}

TEST(Path, TransformedBoundsScaling) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(-1.0, -1.0))
                    .lineTo(Vector2d(1.0, -1.0))
                    .lineTo(Vector2d(1.0, 1.0))
                    .lineTo(Vector2d(-1.0, 1.0))
                    .closePath()
                    .build();

  const Transform2d scalingTransform = Transform2d::Scale(2.0);
  const Box2d expectedBounds(Vector2d(-2.0, -2.0), Vector2d(2.0, 2.0));

  EXPECT_EQ(spline.transformedBounds(scalingTransform), expectedBounds);
}

TEST(Path, TransformedBoundsComplexTransform) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .curveTo(Vector2d(1.0, 2.0), Vector2d(3.0, 2.0), Vector2d(4.0, 0.0))
                    .build();

  const Transform2d complexTransform =
      Transform2d::Scale(0.5) *
      Transform2d::Rotate(MathConstants<double>::kHalfPi)  // Rotate by 90 degrees
      * Transform2d::Translate(2.0, -1.0);
  const Box2d transformedBounds = spline.transformedBounds(complexTransform);

  EXPECT_THAT(transformedBounds.topLeft, Vector2Near(1.25, -1));
  EXPECT_THAT(transformedBounds.bottomRight, Vector2Near(2, 1));
}

TEST(Path, TransformedBoundsEmptySpline) {
  Path spline;
  const Transform2d anyTransform = Transform2d();
  // Path::transformedBounds() returns a default Box2d for an empty path.
  EXPECT_EQ(spline.transformedBounds(anyTransform), Box2d());
}

/**
 * @test that the bounds of a path with a degenerate x-extrema are correctly transformed.
 */
TEST(Path, TransformedBoundsDegenerateXExtrema) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .curveTo(Vector2d(1.0, 0.0), Vector2d(1.0, 0.0), Vector2d(0.0, 0.0))
                    .build();

  // In the original coordinate space the curve reaches a maximum point at t=0.5
  EXPECT_THAT(spline.pointAt(1, 0.5), Vector2Near(0.75, 0.0));

  // Apply a 90-degree rotation about the origin.
  const Transform2d rotation90 = Transform2d::Rotate(MathConstants<double>::kHalfPi);
  const Box2d bounds = spline.transformedBounds(rotation90);
  EXPECT_THAT(bounds, BoxEq(Vector2Near(0.0, 0.0), Vector2d(0.0, 0.75)));
}

/**
 * @test that the bounds of a path with a degenerate y-extrema are correctly transformed.
 */
TEST(Path, TransformedBoundsDegenerateYExtrema) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .curveTo(Vector2d(0.0, 1.0), Vector2d(0.0, 1.0), Vector2d(0.0, 0.0))
                    .build();

  // In the original coordinate space the curve reaches a maximum point at t=0.5
  EXPECT_THAT(spline.pointAt(1, 0.5), Vector2Near(0.0, 0.75));

  // Apply a 90-degree rotation about the origin (rotation: (x,y) -> (-y,x)).
  const Transform2d rotation90 = Transform2d::Rotate(MathConstants<double>::kHalfPi);
  const Box2d bounds = spline.transformedBounds(rotation90);

  EXPECT_THAT(bounds, BoxEq(Vector2d(-0.75, 0.0), Vector2Near(0.0, 0.0)));
}

TEST(Path, StrokeMiterBounds) {
  const double kXHalfExtent = 100.0 / sqrt(3.0);
  const Vector2 kBottomLeft = Vector2(-kXHalfExtent, 0.0);
  const Vector2 kBottomRight = Vector2(kXHalfExtent, 0.0);

  Path spline = PathBuilder()
                    .moveTo(kBottomLeft)
                    .lineTo(Vector2(0.0, 100.0))
                    .lineTo(kBottomRight)
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Box2d kBoundsWithoutMiter = Box2d(kBottomLeft, Vector2d(kXHalfExtent, 100.0));
  const double kExpectedCutoff = 10.0;

  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0),
              BoxEq(kBottomLeft, Vector2Eq(kXHalfExtent, DoubleNear(110.0, 0.01))));
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff + 0.1),
              BoxEq(kBottomLeft, Vector2Eq(kXHalfExtent, DoubleNear(110.0, 0.01))));
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff - 0.1), kBoundsWithoutMiter);
}

TEST(Path, StrokeMiterBoundsClosePath) {
  const double kXHalfExtent = 100.0 / sqrt(3.0);
  const Vector2 kBottomLeft = Vector2(-kXHalfExtent, 0.0);
  const Vector2 kBottomRight = Vector2(kXHalfExtent, 0.0);

  Path spline = PathBuilder()
                    .moveTo(kBottomLeft)
                    .lineTo(Vector2(0.0, 100.0))
                    .lineTo(kBottomRight)
                    .closePath()
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(4));

  const Box2d kBoundsWithoutMiter = Box2d(kBottomLeft, Vector2d(kXHalfExtent, 100.0));
  const double kExpectedCutoff = 10.0;

  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);

  const double kBottomMiterX = 8.66027;
  auto matchSizeWithMiter = BoxEq(Vector2Near(-kXHalfExtent - kBottomMiterX, -5.0),
                                  Vector2Near(kXHalfExtent + kBottomMiterX, 110.0));

  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), matchSizeWithMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff + 0.1), matchSizeWithMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, kExpectedCutoff - 0.1), kBoundsWithoutMiter);
}

TEST(Path, StrokeMiterBoundsColinear) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d::Zero())
                    .lineTo(Vector2d(0.0, 50.0))
                    .lineTo(Vector2d(0.0, 100.0))
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Box2d kBoundsWithoutMiter = Box2d(Vector2d::Zero(), Vector2d(0.0, 100.0));

  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 4.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), kBoundsWithoutMiter);
}

TEST(Path, StrokeMiterBoundsInfinite) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d::Zero())
                    .lineTo(Vector2d(0.0, 100.0))
                    .lineTo(Vector2d::Zero())
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(3));

  const Box2d kBoundsWithoutMiter = Box2d(Vector2d::Zero(), Vector2d(0.0, 100.0));

  EXPECT_EQ(spline.bounds(), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 0.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 4.0), kBoundsWithoutMiter);
  EXPECT_THAT(spline.strokeMiterBounds(5.0, 100.0), kBoundsWithoutMiter);
}

TEST(Path, PointAtTriangle) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2(1.0, 2.0))
                    .lineTo(Vector2(2.0, 0.0))
                    .closePath()
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(4));

  EXPECT_EQ(spline.commands()[0].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(0, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(0, 1.0), Vector2(0.0, 0.0));

  EXPECT_EQ(spline.commands()[1].verb, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(1, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 0.5), Vector2(0.5, 1.0));
  EXPECT_EQ(spline.pointAt(1, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[2].verb, CommandType::LineTo);

  EXPECT_EQ(spline.commands()[3].verb, CommandType::ClosePath);
  EXPECT_EQ(spline.pointAt(3, 0.0), Vector2(2.0, 0.0));
  EXPECT_EQ(spline.pointAt(3, 0.5), Vector2(1.0, 0.0));
  EXPECT_EQ(spline.pointAt(3, 1.0), Vector2(0.0, 0.0));
}

TEST(Path, PointAtMultipleSegments) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2(2.0, 0.0))
                    .moveTo(Vector2(1.0, 1.0))
                    .lineTo(Vector2(1.0, 3.0))
                    .build();

  ASSERT_THAT(spline.commands(), SizeIs(4));

  EXPECT_EQ(spline.commands()[0].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(0, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(0, 1.0), Vector2(0.0, 0.0));

  EXPECT_EQ(spline.commands()[1].verb, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(1, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 0.5), Vector2(1.0, 0.0));
  EXPECT_EQ(spline.pointAt(1, 1.0), Vector2(2.0, 0.0));

  EXPECT_EQ(spline.commands()[2].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(2, 0.0), Vector2(1.0, 1.0));
  EXPECT_EQ(spline.pointAt(2, 1.0), Vector2(1.0, 1.0));

  EXPECT_EQ(spline.commands()[3].verb, CommandType::LineTo);
  EXPECT_EQ(spline.pointAt(3, 0.0), Vector2(1.0, 1.0));
  EXPECT_EQ(spline.pointAt(3, 0.5), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.pointAt(3, 1.0), Vector2(1.0, 3.0));
}

TEST(Path, TangentAt) {
  PathBuilder builder;
  builder.moveTo(Vector2d(0.0, 0.0));
  builder.lineTo(Vector2(1.0, 2.0));
  builder.lineTo(Vector2(2.0, 0.0));
  builder.closePath();
  builder.addCircle(Vector2d(4.0, 1.0), 1.0);
  Path spline = builder.build();

  ASSERT_THAT(spline.commands(), SizeIs(10));

  EXPECT_EQ(spline.commands()[0].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.tangentAt(0, 0.0), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(0, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[1].verb, CommandType::LineTo);
  EXPECT_EQ(spline.tangentAt(1, 0.0), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(1, 0.5), Vector2(1.0, 2.0));
  EXPECT_EQ(spline.tangentAt(1, 1.0), Vector2(1.0, 2.0));

  EXPECT_EQ(spline.commands()[2].verb, CommandType::LineTo);
  EXPECT_EQ(spline.tangentAt(2, 0.0), Vector2(1.0, -2.0));
  EXPECT_EQ(spline.tangentAt(2, 1.0), Vector2(1.0, -2.0));

  EXPECT_EQ(spline.commands()[3].verb, CommandType::ClosePath);
  EXPECT_EQ(spline.tangentAt(3, 0.0), Vector2(-2.0, 0.0));
  EXPECT_EQ(spline.tangentAt(3, 1.0), Vector2(-2.0, 0.0));

  EXPECT_EQ(spline.commands()[4].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(4, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.tangentAt(4, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.tangentAt(4, 1.0), Vector2Eq(0.0, Gt(0.0)));

  EXPECT_EQ(spline.commands()[5].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(5, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.tangentAt(5, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.tangentAt(5, 0.5), NormalizedEq(Vector2(-1.0, 1.0)));
  EXPECT_THAT(spline.tangentAt(5, 1.0), Vector2Eq(Lt(0.0), 0.0));

  EXPECT_EQ(spline.commands()[6].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(6, 0.0), Vector2(4.0, 2.0));
  EXPECT_THAT(spline.tangentAt(6, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.tangentAt(6, 0.5), NormalizedEq(Vector2(-1.0, -1.0)));
  EXPECT_THAT(spline.tangentAt(6, 1.0), Vector2Eq(0.0, Lt(0.0)));

  EXPECT_EQ(spline.commands()[7].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(7, 0.0), Vector2(3.0, 1.0));
  EXPECT_THAT(spline.tangentAt(7, 0.0), Vector2Eq(0.0, Lt(0.0)));
  EXPECT_THAT(spline.tangentAt(7, 0.5), NormalizedEq(Vector2(1.0, -1.0)));
  EXPECT_THAT(spline.tangentAt(7, 1.0), Vector2Eq(Gt(0.0), 0.0));

  EXPECT_EQ(spline.commands()[8].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(8, 0.0), Vector2(4.0, 0.0));
  EXPECT_THAT(spline.tangentAt(8, 0.0), Vector2Eq(Gt(0.0), 0.0));
  EXPECT_THAT(spline.tangentAt(8, 0.5), NormalizedEq(Vector2(1.0, 1.0)));
  EXPECT_THAT(spline.tangentAt(8, 1.0), Vector2Eq(0.0, Gt(0.0)));

  EXPECT_EQ(spline.commands()[9].verb, CommandType::ClosePath);
  EXPECT_EQ(spline.tangentAt(9, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.tangentAt(9, 1.0), Vector2(0.0, 0.0));
}

TEST(Path, TangentAtDegenerateCurve) {
  const Vector2d start(0, 0), degenerate(0, 0), end(1, 0);
  Path spline = PathBuilder().moveTo(start).curveTo(degenerate, degenerate, end).build();

  const Vector2d tangent0 = spline.tangentAt(1, 0.0);
  const Vector2d tangentAdjusted = spline.tangentAt(1, 0.01);
  EXPECT_THAT(tangent0.x, DoubleNear(tangentAdjusted.x, 1e-6));
  EXPECT_THAT(tangent0.y, DoubleNear(tangentAdjusted.y, 1e-6));

  EXPECT_NEAR(tangent0.x, 0.0003, 1e-6);
  EXPECT_NEAR(tangent0.y, 0.0, 1e-6);
}

TEST(Path, TangentAtSingleMoveTo) {
  Path spline = PathBuilder().moveTo(Vector2d(5, 5)).build();
  EXPECT_EQ(spline.tangentAt(0, 0.0), Vector2d::Zero());
}

TEST(Path, NormalAt) {
  PathBuilder builder;
  builder.moveTo(Vector2d(0.0, 0.0));
  builder.lineTo(Vector2(1.0, 2.0));
  builder.lineTo(Vector2(2.0, 0.0));
  builder.closePath();
  builder.addCircle(Vector2d(4.0, 1.0), 1.0);
  Path spline = builder.build();

  ASSERT_THAT(spline.commands(), SizeIs(10));

  EXPECT_EQ(spline.commands()[0].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.normalAt(0, 0.0), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(0, 1.0), Vector2(-2.0, 1.0));

  EXPECT_EQ(spline.commands()[1].verb, CommandType::LineTo);
  EXPECT_EQ(spline.normalAt(1, 0.0), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(1, 0.5), Vector2(-2.0, 1.0));
  EXPECT_EQ(spline.normalAt(1, 1.0), Vector2(-2.0, 1.0));

  EXPECT_EQ(spline.commands()[2].verb, CommandType::LineTo);
  EXPECT_EQ(spline.normalAt(2, 0.0), Vector2(2.0, 1.0));
  EXPECT_EQ(spline.normalAt(2, 1.0), Vector2(2.0, 1.0));

  EXPECT_EQ(spline.commands()[3].verb, CommandType::ClosePath);
  EXPECT_EQ(spline.normalAt(3, 0.0), Vector2(0.0, -2.0));
  EXPECT_EQ(spline.normalAt(3, 1.0), Vector2(0.0, -2.0));

  EXPECT_EQ(spline.commands()[4].verb, CommandType::MoveTo);
  EXPECT_EQ(spline.pointAt(4, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.normalAt(4, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(4, 1.0), Vector2Eq(Lt(0.0), 0.0));

  EXPECT_EQ(spline.commands()[5].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(5, 0.0), Vector2(5.0, 1.0));
  EXPECT_THAT(spline.normalAt(5, 0.0), Vector2Eq(Lt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(5, 0.5), NormalizedEq(Vector2(-1.0, -1.0)));
  EXPECT_THAT(spline.normalAt(5, 1.0), Vector2Eq(0.0, Lt(0.0)));

  EXPECT_EQ(spline.commands()[6].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(6, 0.0), Vector2(4.0, 2.0));
  EXPECT_THAT(spline.normalAt(6, 0.0), Vector2Eq(0.0, Lt(0.0)));
  EXPECT_THAT(spline.normalAt(6, 0.5), NormalizedEq(Vector2(1.0, -1.0)));
  EXPECT_THAT(spline.normalAt(6, 1.0), Vector2Eq(Gt(0.0), 0.0));

  EXPECT_EQ(spline.commands()[7].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(7, 0.0), Vector2(3.0, 1.0));
  EXPECT_THAT(spline.normalAt(7, 0.0), Vector2Eq(Gt(0.0), 0.0));
  EXPECT_THAT(spline.normalAt(7, 0.5), NormalizedEq(Vector2(1.0, 1.0)));
  EXPECT_THAT(spline.normalAt(7, 1.0), Vector2Eq(0.0, Gt(0.0)));

  EXPECT_EQ(spline.commands()[8].verb, CommandType::CurveTo);
  EXPECT_EQ(spline.pointAt(8, 0.0), Vector2(4.0, 0.0));
  EXPECT_THAT(spline.normalAt(8, 0.0), Vector2Eq(0.0, Gt(0.0)));
  EXPECT_THAT(spline.normalAt(8, 0.5), NormalizedEq(Vector2(-1.0, 1.0)));
  EXPECT_THAT(spline.normalAt(8, 1.0), Vector2Eq(Lt(0.0), 0.0));

  EXPECT_EQ(spline.commands()[9].verb, CommandType::ClosePath);
  EXPECT_EQ(spline.normalAt(9, 0.0), Vector2(0.0, 0.0));
  EXPECT_EQ(spline.normalAt(9, 1.0), Vector2(0.0, 0.0));
}

TEST(Path, IsInsideSimpleTriangle) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2d(2.0, 0.0))
                    .lineTo(Vector2d(1.0, 2.0))
                    .closePath()
                    .build();

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

TEST(Path, IsInsideComplexShape) {
  // Two squares, one inside the other
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2d(4.0, 0.0))
                    .lineTo(Vector2d(4.0, 4.0))
                    .lineTo(Vector2d(0.0, 4.0))
                    .closePath()
                    .moveTo(Vector2d(1.0, 1.0))
                    .lineTo(Vector2d(3.0, 1.0))
                    .lineTo(Vector2d(3.0, 3.0))
                    .lineTo(Vector2d(1.0, 3.0))
                    .closePath()
                    .build();

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

TEST(Path, IsInsideCurveShape) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .curveTo(Vector2d(1.0, 2.0), Vector2d(3.0, 2.0), Vector2d(4.0, 0.0))
                    .curveTo(Vector2d(3.0, -2.0), Vector2d(1.0, -2.0), Vector2d(0.0, 0.0))
                    .closePath()
                    .build();

  // Point inside the curve
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 0.0), FillRule::NonZero));
  EXPECT_TRUE(spline.isInside(Vector2d(2.0, 0.0), FillRule::EvenOdd));

  // Point outside the curve
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 0.0), FillRule::NonZero));
  EXPECT_FALSE(spline.isInside(Vector2d(5.0, 0.0), FillRule::EvenOdd));
}

TEST(Path, IsInsideCircle) {
  Path spline = PathBuilder().addCircle(Vector2d(0.0, 0.0), 5.0).build();

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

TEST(Path, IsInsideMultipleSubpaths) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .lineTo(Vector2d(4.0, 0.0))
                    .lineTo(Vector2d(4.0, 4.0))
                    .lineTo(Vector2d(0.0, 4.0))
                    .closePath()
                    .moveTo(Vector2d(5.0, 5.0))
                    .lineTo(Vector2d(7.0, 5.0))
                    .lineTo(Vector2d(7.0, 7.0))
                    .lineTo(Vector2d(5.0, 7.0))
                    .closePath()
                    .build();

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

// TODO(geode): Port appendJoin to Path, then re-enable these tests.
#if 0
TEST(Path, AppendJoin) {
  Path spline1 = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();
  Path spline2 = PathBuilder().moveTo(kVec2).lineTo(kVec3).build();

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}));
}

TEST(Path, AppendJoinWithJump) {
  Path spline1 = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();
  Path spline2 = PathBuilder().moveTo(kVec3).lineTo(kVec4).build();

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec4));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}));
}

TEST(Path, AppendJoinEmpty) {
  Path spline = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();
  Path emptySpline;
  spline.appendJoin(emptySpline);

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command(CommandType::MoveTo, 0), Command(CommandType::LineTo, 1)));
}

TEST(Path, AppendJoinWithMultipleMoveTo) {
  Path spline1 = PathBuilder().moveTo(kVec1).lineTo(kVec2).build();
  Path spline2 = PathBuilder()
                     .moveTo(kVec2)
                     .lineTo(kVec3)
                     .moveTo(kVec4)
                     .lineTo(kVec1)
                     .build();

  spline1.appendJoin(spline2);

  EXPECT_THAT(spline1.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1));
  EXPECT_THAT(spline1.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::LineTo, 2}, Command{CommandType::MoveTo, 3},
                          Command{CommandType::LineTo, 4}));
}
#endif

TEST(Path, VerticesSimple) {
  Path spline =
      PathBuilder().moveTo(kVec1).lineTo(kVec2).lineTo(kVec3).lineTo(kVec4).build();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec4));
}

TEST(Path, VerticesOstreamOutput) {
  Path spline = PathBuilder().moveTo(Vector2d(0, 0)).lineTo(Vector2d(3.0, 4.0)).build();

  EXPECT_THAT(spline.vertices(), ToStringIs("{ Vertex(point=(0, 0), orientation=(0.6, 0.8)), "
                                            "Vertex(point=(3, 4), orientation=(0.6, 0.8)) }"));
}

TEST(Path, VerticesWithJump) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .moveTo(kVec3)
                    .lineTo(kVec4)
                    .build();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec4));
}

TEST(Path, VerticesClosePath) {
  Path spline =
      PathBuilder().moveTo(kVec1).lineTo(kVec2).lineTo(kVec3).closePath().build();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec3, kVec1));
}

TEST(Path, VerticesClosePathWithoutLine) {
  Path spline = PathBuilder()
                    .moveTo(kVec1)
                    .lineTo(kVec2)
                    .moveTo(kVec1)
                    .closePath()
                    .build();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(kVec1, kVec2, kVec1));
}

TEST(Path, VerticesCircle) {
  Path spline = PathBuilder().addCircle(Vector2d(0.0, 0.0), 5.0).build();
  EXPECT_THAT(spline.vertices(),
              VertexPointsAre(Vector2d(5.0, 0.0), Vector2d(0.0, 5.0), Vector2d(-5.0, 0.0),
                              Vector2d(0.0, -5.0), Vector2d(5.0, 0.0)));
}

TEST(Path, VerticesArc) {
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0.0, 0.0))
                    .arcTo(Vector2d(5.0, 5.0), 0.0, /*largeArcFlag=*/true, /*sweepFlag=*/true,
                           Vector2d(5.0, 0.0))
                    .build();

  EXPECT_THAT(spline.vertices(), VertexPointsAre(Vector2d(0.0, 0.0), Vector2Near(5.0, 0.0)));
}

/// @test that isOnPath works for a simple line segment.
TEST(Path, IsOnPathLine) {
  // Create a simple horizontal line from (0,0) to (10,0)
  Path spline = PathBuilder().moveTo(Vector2d(0, 0)).lineTo(Vector2d(10, 0)).build();

  // Exactly on the line.
  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0), 0.001));

  // Within stroke tolerance.
  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0.05), 0.1));

  // Outside the stroke tolerance.
  EXPECT_FALSE(spline.isOnPath(Vector2d(5, 0.2), 0.1));
}

/// @test that isOnPath works for a cubic Bezier curve.
TEST(Path, IsOnPathCurve) {
  // Create a cubic Bezier curve:
  // p0 = (0,0), p1 = (5,0), p2 = (5,10), p3 = (0,10)
  // The midpoint of this curve can be computed (for t=0.5) as:
  // B(0.5) = (3.75, 5.0)
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .curveTo(Vector2d(5, 0), Vector2d(5, 10), Vector2d(0, 10))
                    .build();

  // Test a point exactly on the curve (at t=0.5).
  EXPECT_TRUE(spline.isOnPath(Vector2d(3.75, 5.0), 0.1));

  // Test a point that is near—but not on—the curve.
  EXPECT_FALSE(spline.isOnPath(Vector2d(3.9, 5.0), 0.1));
}

/// @test isOnPath for multiple line segments.
TEST(Path, IsOnPathMultiSegment) {
  // Create a triangle:
  //   (0,0) -> (5,0) -> (2.5,5) -> back to (0,0)
  Path spline = PathBuilder()
                    .moveTo(Vector2d(0, 0))
                    .lineTo(Vector2d(5, 0))
                    .lineTo(Vector2d(2.5, 5))
                    .closePath()
                    .build();

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
TEST(Path, IsOnPathMoveToOnly) {
  // When the path contains only a MoveTo, there is no segment to be on.
  Path spline = PathBuilder().moveTo(Vector2d(1, 1)).build();
  EXPECT_FALSE(spline.isOnPath(Vector2d(1, 1), 0.1));
}

/// @test isOnPath with no stroke width.
TEST(Path, IsOnPathZeroStrokeWidth) {
  // With a strokeWidth of zero, only a point exactly on the segment qualifies.
  Path spline = PathBuilder().moveTo(Vector2d(0, 0)).lineTo(Vector2d(10, 0)).build();

  EXPECT_TRUE(spline.isOnPath(Vector2d(5, 0), 0.0));
  // Even a very small deviation should fail.
  EXPECT_FALSE(spline.isOnPath(Vector2d(5, 0.0001), 0.0));
}

}  // namespace donner::svg
