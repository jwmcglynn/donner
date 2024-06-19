#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include <limits>

#include "donner/base/MathUtils.h"

namespace donner {

TEST(MathUtils, Min) {
  // Base cases.
  EXPECT_EQ(Min(1, 2), 1);
  EXPECT_EQ(Min(-1, 1), -1);
  EXPECT_EQ(Min(0, 0), 0);

  // Extremes.
  EXPECT_EQ(Min(INT32_MIN, INT32_MAX), INT32_MIN);
  EXPECT_EQ(Min(UINT32_MAX, 0u), 0);

  // Many values.
  EXPECT_EQ(Min(1, 2, 3), 1);
  EXPECT_EQ(Min(-1, -2, -3, -100), -100);
}

TEST(MathUtils, Max) {
  // Base cases.
  EXPECT_EQ(Max(1, 2), 2);
  EXPECT_EQ(Max(-1, 1), 1);
  EXPECT_EQ(Max(0, 0), 0);

  // Extremes.
  EXPECT_EQ(Max(INT32_MIN, INT32_MAX), INT32_MAX);
  EXPECT_EQ(Max(UINT32_MAX, 0u), UINT32_MAX);

  // Many values.
  EXPECT_EQ(Max(1, 2, 3), 3);
  EXPECT_EQ(Max(-1, -2, -3, -100), -1);
}

TEST(MathUtils, Abs) {
  EXPECT_EQ(Abs(0), 0);
  EXPECT_EQ(Abs(-1), 1);
  EXPECT_EQ(Abs(UINT32_MAX), UINT32_MAX);
  EXPECT_EQ(Abs(-INT32_MAX), INT32_MAX);
  EXPECT_EQ(Abs(std::numeric_limits<int>::min()), std::numeric_limits<int>::max());

  // Edge case, min value cannot be represented exactly, clips to max.
  EXPECT_EQ(Abs(INT32_MIN), INT32_MAX);

  EXPECT_EQ(Abs(-1.0f), 1.0f);
  EXPECT_EQ(Abs(-std::numeric_limits<float>::infinity()), std::numeric_limits<float>::infinity());

  EXPECT_EQ(Abs(-1.0), 1.0);
  EXPECT_EQ(Abs(-std::numeric_limits<double>::infinity()), std::numeric_limits<double>::infinity());

  // Edge case, numeric_limits<float>::min() is the minimum *positive* value.
  EXPECT_EQ(Abs(std::numeric_limits<float>::min()), std::numeric_limits<float>::min());
}

TEST(MathUtils, Round) {
  EXPECT_EQ(Round(1.0f), 1.0f);
  EXPECT_EQ(Round(1.5f), 2.0f);
  EXPECT_EQ(Round(1.6f), 2.0f);
  EXPECT_EQ(Round(1.49f), 1.0f);

  EXPECT_EQ(Round(5.0), 5.0);
  EXPECT_EQ(Round(100.1), 100.0);
  EXPECT_EQ(Round(100.49), 100.0);
  EXPECT_EQ(Round(100.5), 101.0);

  EXPECT_EQ(Round(std::numeric_limits<float>::infinity()), std::numeric_limits<float>::infinity());
  EXPECT_EQ(Round(std::numeric_limits<double>::infinity()),
            std::numeric_limits<double>::infinity());

  // Negative values round towards zero.
  EXPECT_EQ(Round(-0.5f), 0.0f);
  EXPECT_EQ(Round(-0.51f), -1.0f);
  EXPECT_EQ(Round(-0.1f), 0.0f);
  EXPECT_EQ(Round(-0.9f), -1.0f);

  EXPECT_EQ(Round(-0.5), 0.0);
  EXPECT_EQ(Round(-0.51), -1.0);
  EXPECT_EQ(Round(-0.1), 0.0);
  EXPECT_EQ(Round(-0.9), -1.0);

  EXPECT_EQ(Round(-std::numeric_limits<float>::infinity()),
            -std::numeric_limits<float>::infinity());
  EXPECT_EQ(Round(-std::numeric_limits<double>::infinity()),
            -std::numeric_limits<double>::infinity());
}

TEST(MathUtils, Lerp) {
  EXPECT_FLOAT_EQ(Lerp(0.0f, 1.0f, 0.5f), 0.5f);
  EXPECT_FLOAT_EQ(Lerp(0.0f, 120.0f, 0.2f), 24.0f);

  EXPECT_FLOAT_EQ(Lerp(-100.0f, 100.0f, 0.0f), -100.0f);
  EXPECT_FLOAT_EQ(Lerp(-100.0f, 100.0f, 0.5f), 0.0f);
  EXPECT_FLOAT_EQ(Lerp(-100.0f, 100.0f, 1.0f), 100.0f);

  EXPECT_DEBUG_DEATH(Lerp(0.0f, 1.0f, -0.01f), "");
  EXPECT_DEBUG_DEATH(Lerp(0.0f, 1.0f, 1.01f), "");

  // Double.
  EXPECT_FLOAT_EQ(Lerp(0.0, 1.0, 0.5), 0.5);
  EXPECT_FLOAT_EQ(Lerp(0.0, 120.0, 0.2), 24.0);
}

TEST(MathUtils, Clamp) {
  EXPECT_EQ(Clamp(123, 0, 100), 100);
  EXPECT_EQ(Clamp(50, 0, 100), 50);
  EXPECT_EQ(Clamp(-100, 0, 1), 0);
}

TEST(MathUtils, NearEquals) {
  EXPECT_TRUE(NearEquals(0.0f, 0.0f));
  EXPECT_FALSE(NearEquals(0.0f, 1.0f));
  EXPECT_TRUE(NearEquals(0.0, 0.0));
  EXPECT_FALSE(NearEquals(0.0, 1.0));

  EXPECT_TRUE(NearEquals(std::numeric_limits<float>::infinity(),
                         std::numeric_limits<float>::infinity() * 2.0f));
  EXPECT_TRUE(NearEquals(25.0f * 10.0f / 123.0f, 25.0f / 123.0f * 10.0f));

  EXPECT_FALSE(NearEquals(100.0f, 101.0f, 0.5f));
  EXPECT_FALSE(NearEquals(100.0f, 101.0f, 0.9f));
  EXPECT_TRUE(NearEquals(100.0f, 101.0f, 1.0f));

  EXPECT_FALSE(
      NearEquals(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()));
}

TEST(MathUtils, InRange) {
  EXPECT_TRUE(InRange('a', 'a', 'z'));
  EXPECT_TRUE(InRange('m', 'a', 'z'));
  EXPECT_TRUE(InRange('z', 'a', 'z'));
  EXPECT_FALSE(InRange('(', 'a', 'z'));

  EXPECT_TRUE(InRange(5, 1, 7));
  EXPECT_TRUE(InRange(5u, 1u, 7u));
  EXPECT_FALSE(InRange(10, 1, 7));
  EXPECT_FALSE(InRange(10u, 1u, 7u));
  EXPECT_FALSE(InRange(10, 12, 14));
  EXPECT_FALSE(InRange(10u, 12u, 14u));

  // Full range.
  EXPECT_TRUE(InRange(uint32_t(8), uint32_t(0), uint32_t(255)));

  // Invalid inputs.
  EXPECT_DEBUG_DEATH({ InRange(10, 12, 7); }, "start <= end");
  EXPECT_DEBUG_DEATH({ InRange(10u, 12u, 7u); }, "start <= end");
  EXPECT_DEBUG_DEATH({ InRange('a', 'z', 'a'); }, "start <= end");
}

TEST(MathUtils, SolveQuadratic) {
  {
    QuadraticSolution<float> res = SolveQuadratic(0.0f, 0.0f, 0.0f);
    EXPECT_FALSE(res.hasSolution);
  }

  {
    QuadraticSolution<float> res = SolveQuadratic(1.0f, 1.0f, 1.0f);
    EXPECT_FALSE(res.hasSolution);
  }

  {
    QuadraticSolution<float> res = SolveQuadratic(1.0f, 2.0f, 1.0f);
    EXPECT_TRUE(res.hasSolution);
    EXPECT_EQ(res.solution[0], -1.0f);
    EXPECT_EQ(res.solution[1], -1.0f);
  }

  {
    QuadraticSolution<float> res = SolveQuadratic(1.0f, 5.0f, 2.25f);
    EXPECT_TRUE(res.hasSolution);
    EXPECT_EQ(res.solution[0], -0.5f);
    EXPECT_EQ(res.solution[1], -4.5f);
  }
}

}  // namespace donner
