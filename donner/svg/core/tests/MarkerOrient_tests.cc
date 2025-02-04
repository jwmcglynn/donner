#include "donner/svg/core/MarkerOrient.h"

#include <gtest/gtest.h>

#include <cmath>

#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/// @test Default–constructed MarkerOrient produces an Angle–type with a zero angle.
TEST(MarkerOrientTests, DefaultConstructor) {
  const MarkerOrient orient;
  EXPECT_EQ(orient.type(), MarkerOrient::Type::Angle);

  // For any direction, since the angle is fixed (0.0), computeAngleRadians should return 0.
  const Vector2d direction(1.0, 1.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction), 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction, MarkerOrient::MarkerType::Start), 0.0);
}

/// @test Verify that \c MarkerOrient::AngleRadians produces an orientation with the given angle.
TEST(MarkerOrientTests, AngleRadians) {
  const double testAngle = 1.234;  // in radians
  const MarkerOrient orient = MarkerOrient::AngleRadians(testAngle);
  EXPECT_EQ(orient.type(), MarkerOrient::Type::Angle);

  // Regardless of the direction, the computed angle remains the given angle.
  const Vector2d direction(0.0, 1.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction), testAngle);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction, MarkerOrient::MarkerType::Start),
                   testAngle);
}

/// @test Verify that \c MarkerOrient::AngleDegrees converts degrees to radians correctly.
TEST(MarkerOrientTests, AngleDegrees) {
  const double angleDegrees = 180.0;
  const MarkerOrient orient = MarkerOrient::AngleDegrees(angleDegrees);
  EXPECT_EQ(orient.type(), MarkerOrient::Type::Angle);

  const double expectedRadians = angleDegrees * MathConstants<double>::kDegToRad;
  const Vector2d direction(1.0, 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction), expectedRadians);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(direction, MarkerOrient::MarkerType::Start),
                   expectedRadians);
}

/// @test Verify that \c MarkerOrient::Auto computes the angle from the provided direction vector.
TEST(MarkerOrientTests, AutoOrientation) {
  const MarkerOrient orient = MarkerOrient::Auto();
  EXPECT_EQ(orient.type(), MarkerOrient::Type::Auto);

  // With a horizontal direction, the computed angle should be 0.
  const Vector2d horizontal(1.0, 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(horizontal), 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(horizontal, MarkerOrient::MarkerType::Start), 0.0);

  // With a vertical direction, the computed angle should be π/2.
  Vector2d vertical(0.0, 1.0);
  const double expectedAngle = std::atan2(vertical.y, vertical.x);  // π/2
  EXPECT_NEAR(orient.computeAngleRadians(vertical), expectedAngle, 1e-6);

  // With a near–zero direction vector, the computed angle should fall back to 0.
  const Vector2d nearZero(0.0, 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(nearZero), 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(nearZero, MarkerOrient::MarkerType::Start), 0.0);
}

/// @test Verify that \c MarkerOrient::AutoStartReverse adds π to the computed angle when \c
/// MarkerType::Start is set.
TEST(MarkerOrientTests, AutoStartReverseOrientation) {
  const MarkerOrient orient = MarkerOrient::AutoStartReverse();
  EXPECT_EQ(orient.type(), MarkerOrient::Type::AutoStartReverse);

  // With a horizontal direction (angle 0), for a marker start the angle should be π.
  const Vector2d horizontal(1.0, 0.0);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(horizontal, MarkerOrient::MarkerType::Start),
                   0.0 + MathConstants<double>::kPi);
  // For non-marker start, the angle is computed normally.
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(horizontal), 0.0);

  // For a vertical direction (angle π/2)
  const Vector2d vertical(0.0, 1.0);
  const double baseAngle = std::atan2(vertical.y, vertical.x);  // π/2
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(vertical), baseAngle);
  EXPECT_DOUBLE_EQ(orient.computeAngleRadians(vertical, MarkerOrient::MarkerType::Start),
                   baseAngle + MathConstants<double>::kPi);
}

/// @test Equality operator compares both the type and the angle value.
TEST(MarkerOrientTests, EqualityOperator) {
  MarkerOrient a = MarkerOrient::AngleRadians(1.0);
  MarkerOrient b = MarkerOrient::AngleRadians(1.0);
  MarkerOrient c = MarkerOrient::AngleRadians(2.0);
  MarkerOrient d = MarkerOrient::Auto();
  MarkerOrient e = MarkerOrient::Auto();

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_EQ(d, e);
  EXPECT_NE(a, d);
}

}  // namespace donner::svg
