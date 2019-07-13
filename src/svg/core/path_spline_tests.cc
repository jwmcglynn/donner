#include "src/svg/core/path_spline.h"

#include <gmock/gmock.h>
#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

using testing::ElementsAre;
using testing::_;

namespace donner {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

static constexpr size_t kNPos = ~size_t(0);
static constexpr Vector2d kVec1(123.0, 456.7);
static constexpr Vector2d kVec2(78.9, 1011.12);
static constexpr Vector2d kVec3(-1314.0, 1516.17);
static constexpr Vector2d kVec4(1819.0, -2021.22);

static bool operator==(const Command& lhs, const Command& rhs) {
  return lhs.index == rhs.index && lhs.type == rhs.type;
}

std::ostream& operator<<(std::ostream& os, CommandType type) {
  switch (type) {
    case CommandType::MoveTo: os << "CommandType::MoveTo"; break;
    case CommandType::LineTo: os << "CommandType::LineTo"; break;
    case CommandType::CurveTo: os << "CommandType::CurveTo"; break;
    default: UTILS_RELEASE_ASSERT(false && "Invalid command");
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const Command& command) {
  os << "Command {" << command.type << ", " << command.index << "}";
  return os;
}

TEST(PathSplineBuilder, MoveTo) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(), ElementsAre(kVec1));
  EXPECT_THAT(spline.Commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, MoveTo_Replace) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  builder.MoveTo(kVec2);
  PathSpline spline = builder.Build();

  // Only the last command remains.
  EXPECT_THAT(spline.Points(), ElementsAre(kVec2));
  EXPECT_THAT(spline.Commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, LineTo) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  builder.LineTo(kVec2);
  PathSpline spline = builder.Build();

  // Only the last command remains.
  EXPECT_THAT(spline.Points(), ElementsAre(kVec1, kVec2));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
}

TEST(PathSplineBuilder, LineTo_Complex) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(Vector2d::Zero());
  builder.LineTo(kVec1);
  // Create a separate line with two segments.
  builder.MoveTo(Vector2d::Zero());
  builder.LineTo(kVec2);
  builder.LineTo(kVec1);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(),
              ElementsAre(Vector2d::Zero(), kVec1, Vector2d::Zero(), kVec2, kVec1));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3},
                          Command{CommandType::LineTo, 4}));
}

TEST(PathSplineBuilder, LineTo_FailsWithoutStart) {
  auto builder = PathSpline::Builder();
  EXPECT_DEATH(builder.LineTo(kVec1), "without calling MoveTo");
}

TEST(PathSplineBuilder, CurveTo) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  builder.CurveTo(kVec2, kVec3, kVec4);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(), ElementsAre(kVec1, kVec2, kVec3, kVec4));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSplineBuilder, CurveTo_Chained) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  builder.CurveTo(kVec2, kVec3, kVec4);
  builder.CurveTo(kVec1, kVec2, Vector2d::Zero());
  builder.LineTo(kVec1);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(),
              ElementsAre(kVec1, kVec2, kVec3, kVec4, kVec1, kVec2, Vector2d::Zero(), kVec1));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::LineTo, 7}));
}

TEST(PathSplineBuilder, CurveTo_FailsWithoutStart) {
  auto builder = PathSpline::Builder();
  EXPECT_DEATH(builder.CurveTo(kVec1, kVec2, kVec3), "without calling MoveTo");
}

TEST(PathSplineBuilder, ArcTo) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(Vector2d(1.0, 0.0));
  builder.ArcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, false, false,
                Vector2d(0.0, 2.0));
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1}));
}

TEST(PathSplineBuilder, ArcTo_LargeArc) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(Vector2d(1.0, 0.0));
  builder.ArcTo(Vector2d(2.0, 1.0), MathConstants<double>::kHalfPi, true, false,
                Vector2d(0.0, 2.0));
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(), ElementsAre(Vector2d(1.0, 0.0), _, _, Vector2d(0.0, -2.0), _, _, _,
                                           _, _, Vector2d(0.0, 2.0)));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7}));
}

TEST(PathSplineBuilder, ClosePath) {
  auto builder = PathSpline::Builder();
  builder.MoveTo(kVec1);
  builder.LineTo(kVec2);
  builder.ClosePath();
  builder.LineTo(kVec3);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(), ElementsAre(kVec1, kVec2, kVec3));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                          Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 2}));
}

TEST(PathSplineBuilder, Ellipse) {
  auto builder = PathSpline::Builder();
  builder.Ellipse(Vector2d(0.0, 1.0), Vector2d(2.0, 1.0));
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, 0.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 2.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, Circle) {
  auto builder = PathSpline::Builder();
  builder.Circle(Vector2d(0.0, 1.0), 2.0);
  PathSpline spline = builder.Build();

  EXPECT_THAT(spline.Points(),
              ElementsAre(Vector2d(2.0, 1.0), _, _, Vector2d(0.0, -1.0), _, _, Vector2d(-2.0, 1.0),
                          _, _, Vector2d(0.0, 3.0), _, _, Vector2d(2.0, 1.0)));
  EXPECT_THAT(spline.Commands(),
              ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                          Command{CommandType::CurveTo, 4}, Command{CommandType::CurveTo, 7},
                          Command{CommandType::CurveTo, 10}, Command{CommandType::MoveTo, 0}));
}

TEST(PathSplineBuilder, Build_MultipleTimesFails) {
  auto builder = PathSpline::Builder();
  PathSpline spline1 = builder.Build();

  EXPECT_DEATH(builder.Build(), "can only be used once");
}

TEST(PathSpline, Empty) {
  PathSpline spline = PathSpline::Builder().Build();
  EXPECT_TRUE(spline.IsEmpty());
}

TEST(PathSpline, Empty_BoundsFails) {
  PathSpline spline = PathSpline::Builder().Build();
  EXPECT_DEATH(spline.Bounds(), "IsEmpty");
  EXPECT_DEATH(spline.StrokeMiterBounds(1.0, 1.0), "IsEmpty");
}

// TODO: Bounds
// TODO: StrokeMiterBounds
// TODO: PointAt
// TODO: TangentAt
// TODO: NormalAt

}  // namespace donner
