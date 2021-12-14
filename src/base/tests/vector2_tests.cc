#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/vector2.h"

using testing::AnyOf;

namespace donner {

TEST(Vector2, Construct) {
  Vector2f vec_float(5.0f, -1.0f);
  EXPECT_FLOAT_EQ(vec_float.x, 5.0f);
  EXPECT_FLOAT_EQ(vec_float.y, -1.0f);

  Vector2d vec_double(-50.0, 123.0);
  EXPECT_FLOAT_EQ(vec_double.x, -50.0);
  EXPECT_FLOAT_EQ(vec_double.y, 123.0);

  Vector2i vec_int(-123, 123);
  EXPECT_FLOAT_EQ(vec_int.x, -123);
  EXPECT_FLOAT_EQ(vec_int.y, 123);
}

TEST(Vector2, ConstructDefault) {
  Vector2f vec_float;
  EXPECT_FLOAT_EQ(vec_float.x, 0.0f);
  EXPECT_FLOAT_EQ(vec_float.y, 0.0f);

  Vector2d vec_double;
  EXPECT_FLOAT_EQ(vec_double.x, 0.0);
  EXPECT_FLOAT_EQ(vec_double.y, 0.0);

  Vector2i vec_int;
  EXPECT_FLOAT_EQ(vec_int.x, 0);
  EXPECT_FLOAT_EQ(vec_int.y, 0);
}

TEST(Vector2, CastConstruct) {
  Vector2f float_vec(123.4f, 567.8f);

  {
    Vector2d double_vec(float_vec);
    EXPECT_EQ(float_vec, double_vec);
  }

  {
    Vector2d double_vec = float_vec;
    EXPECT_EQ(float_vec, double_vec);
  }

  {
    Vector2d double_vec(float_vec.x, float_vec.y);
    EXPECT_EQ(float_vec, double_vec);
  }
}

TEST(Vector2, Constants) {
  EXPECT_EQ(Vector2f::Zero(), Vector2f(0.0f, 0.0f));
  EXPECT_EQ(Vector2d::Zero(), Vector2d(0.0, 0.0));
  EXPECT_EQ(Vector2i::Zero(), Vector2i(0, 0));

  EXPECT_EQ(Vector2f::XAxis(), Vector2f(1.0f, 0.0f));
  EXPECT_EQ(Vector2d::XAxis(), Vector2d(1.0, 0.0));
  EXPECT_EQ(Vector2i::XAxis(), Vector2i(1, 0));

  EXPECT_EQ(Vector2f::YAxis(), Vector2f(0.0f, 1.0f));
  EXPECT_EQ(Vector2d::YAxis(), Vector2d(0.0, 1.0));
  EXPECT_EQ(Vector2i::YAxis(), Vector2i(0, 1));
}

TEST(Vector2, Length) {
  EXPECT_FLOAT_EQ(Vector2f(0.0f, 1.0f).length(), 1.0f);
  EXPECT_FLOAT_EQ(Vector2f(0.0f, 1.0f).lengthSquared(), 1.0f);
  EXPECT_FLOAT_EQ(Vector2f(3.0f, 4.0f).length(), 5.0f);
  EXPECT_FLOAT_EQ(Vector2f(3.0f, 4.0f).lengthSquared(), 25.0f);

  EXPECT_FLOAT_EQ(Vector2f::Zero().length(), 0.0f);
  EXPECT_FLOAT_EQ(Vector2f::Zero().lengthSquared(), 0.0f);
  EXPECT_FLOAT_EQ(Vector2f(-3.0f, 4.0f).length(), 5.0f);
  EXPECT_FLOAT_EQ(Vector2f(-3.0f, 4.0f).lengthSquared(), 25.0f);
}

TEST(Vector2, Distance) {
  EXPECT_FLOAT_EQ(Vector2f(0.0f, 1.0f).distanceSquared(Vector2f(1.0f, 1.0f)), 1.0f);
  EXPECT_FLOAT_EQ(Vector2f(0.0f, 1.0f).distanceSquared(Vector2f(5.0f, 1.0f)), 25.0f);

  EXPECT_EQ(Vector2i(0, 5).distance(Vector2i(0, -5)), 10);
  EXPECT_EQ(Vector2i(0, 5).distanceSquared(Vector2i(0, -5)), 100);

  // Integers are truncated.
  EXPECT_EQ(Vector2i(0, 0).distance(Vector2i(2, 2)), 2);
  EXPECT_EQ(Vector2i(0, 0).distanceSquared(Vector2i(2, 2)), 8);
}

TEST(Vector2, Dot) {
  EXPECT_EQ(Vector2i::Zero().dot(Vector2i::Zero()), 0);
  EXPECT_EQ(Vector2i::Zero().dot(Vector2i(5, 5)), 0);
  EXPECT_EQ(Vector2i(-2, -2).dot(Vector2i(2, 2)), -8);
  EXPECT_EQ(Vector2i(-2, 1).dot(Vector2i(2, 2)), -2);
}

TEST(Vector2, Rotate) {
  EXPECT_EQ(Vector2f::XAxis().rotate(MathConstants<float>::kHalfPi), Vector2f::YAxis());
  EXPECT_EQ(Vector2f::XAxis().rotate(MathConstants<float>::kPi * 0.25f),
            Vector2f(sqrt(2.0f) * 0.5f, sqrt(2.0f) * 0.5f));
}

TEST(Vector2, Angle) {
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angle(), 0.0f);
  EXPECT_FLOAT_EQ(Vector2f(-1.0f, 0.0f).angle(), +MathConstants<float>::kPi);
  EXPECT_FLOAT_EQ(Vector2f(0.0f, -1.0f).angle(), -MathConstants<float>::kHalfPi);
  EXPECT_FLOAT_EQ(Vector2f(sqrt(2.0f) * 0.5f, sqrt(2.0f) * 0.5f).angle(),
                  MathConstants<float>::kPi * 0.25f);
  EXPECT_FLOAT_EQ(Vector2f(0.0f, 1.0f).angle(), +MathConstants<float>::kHalfPi);
}

TEST(Vector2, AngleWith) {
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f::XAxis()), 0.0f);
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f::YAxis()), MathConstants<float>::kHalfPi);
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(-Vector2f::XAxis()), MathConstants<float>::kPi);

  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f(1.0f, 1.0f)),
                  MathConstants<float>::kPi / 4.0f);
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f(1.0f, -1.0f)),
                  MathConstants<float>::kPi / 4.0f);
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f(-1.0f, 1.0f)),
                  MathConstants<float>::kPi * 3.0f / 4.0f);
  EXPECT_FLOAT_EQ(Vector2f::XAxis().angleWith(Vector2f(-1.0f, -1.0f)),
                  MathConstants<float>::kPi * 3.0f / 4.0f);

  // Edge case: Zero length.
  EXPECT_FLOAT_EQ(Vector2f::Zero().angleWith(Vector2f::Zero()), 0.0f);
}

// Normalize
TEST(Vector2, Normalize) {
  EXPECT_EQ(Vector2f(5.0f, 0.0).normalize(), Vector2f::XAxis());
  EXPECT_EQ(Vector2f(-5.0f, 0.0).normalize(), Vector2f(-1.0f, 0.0f));
  EXPECT_EQ(Vector2f(1.0f, 1.0).normalize(), Vector2f(sqrt(2.0f) * 0.5f, sqrt(2.0f) * 0.5f));
}

TEST(Vector2, NormalizeNearZero) {
  EXPECT_EQ(Vector2f(std::numeric_limits<float>::epsilon(), 0.0f).normalize(), Vector2f::Zero());
}

// Operators
TEST(Vector2, OperatorAssign) {
  Vector2i vec = Vector2i::Zero();
  EXPECT_EQ(vec, Vector2i(0, 0));

  vec = Vector2i(5, 10);
  EXPECT_EQ(vec, Vector2i(5, 10));
}

TEST(Vector2, OperatorUnaryMinus) {
  EXPECT_EQ(-Vector2i(-1, 1), Vector2i(1, -1));
  EXPECT_EQ(-Vector2i::Zero(), Vector2i::Zero());
}

TEST(Vector2, OperatorAdd) {
  EXPECT_EQ(Vector2i(2, -4) + Vector2i(-4, 12), Vector2i(-2, 8));
  EXPECT_EQ(Vector2i(2, -4) + Vector2i::Zero(), Vector2i(2, -4));

  Vector2i vec = Vector2i::Zero();
  vec += Vector2i(5, 10);
  EXPECT_EQ(vec, Vector2i(5, 10));
}

TEST(Vector2, OperatorSubtract) {
  EXPECT_EQ(Vector2i(2, -4) - Vector2i(-4, 12), Vector2i(6, -16));
  EXPECT_EQ(Vector2i(2, -4) - Vector2i::Zero(), Vector2i(2, -4));

  Vector2i vec = Vector2i::Zero();
  vec -= Vector2i(5, 10);
  EXPECT_EQ(vec, Vector2i(-5, -10));
}

TEST(Vector2, OperatorPiecewiseMultiply) {
  EXPECT_EQ(Vector2i(2, -4) * Vector2i(-4, 12), Vector2i(-8, -48));
  EXPECT_EQ(Vector2i(2, -4) * Vector2i::Zero(), Vector2i::Zero());

  Vector2i vec = Vector2i(2, -3);
  vec *= Vector2i(5, 10);
  EXPECT_EQ(vec, Vector2i(10, -30));
}

TEST(Vector2, OperatorPiecewiseDivide) {
  EXPECT_EQ(Vector2i(2, -4) / Vector2i(-2, 2), Vector2i(-1, -2));
  EXPECT_EQ(Vector2i(0, 0) * Vector2i(1, 1), Vector2i::Zero());

  Vector2i vec = Vector2i(2, 8);
  vec /= Vector2i(2, -4);
  EXPECT_EQ(vec, Vector2i(1, -2));
}

TEST(Vector2, OperatorScalarMultiply) {
  EXPECT_EQ(Vector2i(-8, 2) * 2, Vector2i(-16, 4));
  EXPECT_EQ(-3 * Vector2i(-8, 2), Vector2i(24, -6));
}

TEST(Vector2, OperatorScalarDivide) {
  EXPECT_EQ(Vector2i(-8, 2) / 2, Vector2i(-4, 1));
}

TEST(Vector2, Negation) {
  EXPECT_EQ(-Vector2i::Zero(), Vector2i::Zero());
  EXPECT_EQ(-Vector2i(123, -456), Vector2i(-123, 456));
}

TEST(Vector2, Equals) {
  EXPECT_TRUE(Vector2i::Zero() == Vector2i(0, 0));
  EXPECT_FALSE(Vector2i::Zero() == Vector2i(1, 0));
  EXPECT_TRUE(Vector2i::Zero() != Vector2i(123, 456));
  EXPECT_FALSE(Vector2i::Zero() != Vector2i(0, 0));
  EXPECT_TRUE(Vector2i(123, 456) == Vector2i(123, 456));
  EXPECT_FALSE(Vector2i(123, 456) == Vector2i(123, 567));
}

TEST(Vector2, Output) {
  EXPECT_EQ((std::ostringstream() << Vector2i(1, 2)).str(), "(1, 2)");
  EXPECT_EQ((std::ostringstream() << Vector2i(-3, -4)).str(), "(-3, -4)");

  EXPECT_EQ((std::ostringstream() << Vector2d(1.0, 2.0)).str(), "(1, 2)");
  EXPECT_EQ((std::ostringstream() << Vector2d(-1.5, -10)).str(), "(-1.5, -10)");

  EXPECT_EQ((std::ostringstream() << Vector2d(std::numeric_limits<double>::infinity(),
                                              -std::numeric_limits<double>::infinity()))
                .str(),
            "(inf, -inf)");

  EXPECT_THAT((std::ostringstream() << Vector2d(std::numeric_limits<double>::quiet_NaN(),
                                                -std::numeric_limits<double>::quiet_NaN()))
                  .str(),
              AnyOf("(nan, -nan)", "(nan, nan)"));

  EXPECT_EQ((std::ostringstream() << Vector2d(0.0, -0.0)).str(), "(0, -0)");
}

}  // namespace donner