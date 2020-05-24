#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include "src/svg/core/path_spline.h"
#include "src/svg/core/tests/path_spline_test_utils.h"

using testing::_;
using testing::ElementsAre;

namespace donner {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

static constexpr Vector2d kVec1(123.0, 456.7);
static constexpr Vector2d kVec2(78.9, 1011.12);
static constexpr Vector2d kVec3(-1314.0, 1516.17);
static constexpr Vector2d kVec4(1819.0, -2021.22);

static testing::Matcher<Vector2d> MatchVec2d(testing::Matcher<double> x,
                                             testing::Matcher<double> y) {
  return testing::AllOf(testing::Field(&Vector2d::x, x), testing::Field(&Vector2d::y, y));
}
static testing::Matcher<Boxd> MatchBoxd(testing::Matcher<Vector2d> tl,
                                        testing::Matcher<Vector2d> br) {
  return testing::AllOf(testing::Field(&Boxd::top_left, tl),
                        testing::Field(&Boxd::bottom_right, br));
}

TEST(PathSplineBuilder, MoveTo) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, MoveTo_Replace) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  builder.moveTo(kVec2);
  PathSpline spline = builder.build();

  // Only the last command remains.
  EXPECT_THAT(spline.points(), ElementsAre(kVec2));
  EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, LineTo) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  builder.lineTo(kVec2);
  PathSpline spline = builder.build();

  // Only the last command remains.
  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
}

TEST(PathSplineBuilder, LineTo_Complex) {
  auto builder = PathSpline::Builder();
  builder.moveTo(Vector2d::Zero());
  builder.lineTo(kVec1);
  // Create a separate line with two segments.
  builder.moveTo(Vector2d::Zero());
  builder.lineTo(kVec2);
  builder.lineTo(kVec1);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d::Zero(), kVec1, Vector2d::Zero(), kVec2, kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3},
                          Command{CommandType::LineTo, 4}));
}

TEST(PathSplineBuilder, LineTo_FailsWithoutStart) {
  auto builder = PathSpline::Builder();
  EXPECT_DEATH(builder.lineTo(kVec1), "without calling MoveTo");
}

TEST(PathSplineBuilder, CurveTo) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  builder.curveTo(kVec2, kVec3, kVec4);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSplineBuilder, CurveTo_Chained) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  builder.curveTo(kVec2, kVec3, kVec4);
  builder.curveTo(kVec1, kVec2, Vector2d::Zero());
  builder.lineTo(kVec1);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(),
              ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1, kVec2, Vector2d::Zero(), kVec1));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::LineTo, 7}));
}

TEST(PathSplineBuilder, CurveTo_FailsWithoutStart) {
  auto builder = PathSpline::Builder();
  EXPECT_DEATH(builder.curveTo(kVec1, kVec2, kVec3), "without calling MoveTo");
}

TEST(PathSplineBuilder, ArcTo) {
  auto builder = PathSpline::Builder();
  builder.moveTo(Vector2d(1.0, 0.0));
  builder.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
                Vector2d(0.0, 2.0));
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSplineBuilder, ArcTo_LargeArc) {
  auto builder = PathSpline::Builder();
  builder.moveTo(Vector2d(1.0, 0.0));
  builder.arcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, true, false,
                Vector2d(0.0, 2.0));
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, -2.0), _, _, _,
                                           _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7}));
}

TEST(PathSplineBuilder, ClosePath) {
  auto builder = PathSpline::Builder();
  builder.moveTo(kVec1);
  builder.lineTo(kVec2);
  builder.closePath();
  builder.lineTo(kVec3);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 2}));
}

TEST(PathSplineBuilder, Ellipse) {
  auto builder = PathSpline::Builder();
  builder.ellipse(Vector2d(0.0, 1.0), Vector2d(2.0, 1.0));
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 0.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 2.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, Circle) {
  auto builder = PathSpline::Builder();
  builder.circle(Vector2d(0.0, 1.0), 2.0);
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, -1.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 3.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, Build_MultipleTimesFails) {
  auto builder = PathSpline::Builder();
  PathSpline spline1 = builder.build();

  EXPECT_DEATH(builder.build(), "can only be used once");
}

TEST(PathSpline, Empty) {
  PathSpline spline = PathSpline::Builder().build();
  EXPECT_TRUE(spline.empty());
}

TEST(PathSpline, Empty_BoundsFails) {
  PathSpline spline = PathSpline::Builder().build();
  EXPECT_DEATH(spline.bounds(), "!empty()");
  EXPECT_DEATH(spline.strokeMiterBounds(1.0, 1.0), "!empty()");
}

TEST(PathSpline, Bounds) {
  auto builder = PathSpline::Builder();
  builder.moveTo(Vector2d::Zero());
  builder.lineTo(kVec1);
  builder.lineTo(kVec2);
  PathSpline spline = builder.build();

  EXPECT_EQ(spline.bounds(), Boxd(Vector2d(0.0, 0.0), Vector2d(123.0, 1011.12)));
}

TEST(PathSpline, Bounds_Curve) {
  auto builder = PathSpline::Builder();
  builder.moveTo(Vector2d(0.0, 0.0));
  builder.curveTo(Vector2d(8.0, 9.0), Vector2d(2.0, 0.0), Vector2d(0.0, 0.0));
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.bounds(),
              MatchBoxd(Vector2d(0.0, 0.0), MatchVec2d(testing::DoubleNear(4.04307, 0.01),
                                                       testing::DoubleNear(4.0, 0.01))));
}

TEST(PathSpline, Bounds_Ellipse) {
  auto builder = PathSpline::Builder();
  builder.ellipse(Vector2d(1.0, 2.0), Vector2d(2.0, 1.0));
  PathSpline spline = builder.build();

  EXPECT_THAT(spline.bounds(), Boxd(Vector2d(-1.0, 1.0), Vector2d(3.0, 3.0)));
}

// TODO: StrokeMiterBounds
// TODO: PointAt
// TODO: TangentAt
// TODO: NormalAt

}  // namespace donner
