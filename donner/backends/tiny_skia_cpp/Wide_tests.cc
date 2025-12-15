#include "donner/backends/tiny_skia_cpp/Wide.h"

#include <array>

#include <gtest/gtest.h>

#include "donner/backends/tiny_skia_cpp/Color.h"

namespace donner::backends::tiny_skia_cpp {

TEST(WideTest, SplatAndArray) {
  const F32x4 splat = F32x4::Splat(3.5f);
  const std::array<float, 4> values = splat.toArray();
  for (float value : values) {
    EXPECT_FLOAT_EQ(value, 3.5f);
  }
}

TEST(WideTest, AddAndMultiply) {
  const F32x4 lhs = F32x4::FromArray({1.0f, 2.0f, 3.0f, 4.0f});
  const F32x4 rhs = F32x4::FromArray({4.0f, 3.0f, 2.0f, 1.0f});

  const std::array<float, 4> sum = (lhs + rhs).toArray();
  EXPECT_FLOAT_EQ(sum[0], 5.0f);
  EXPECT_FLOAT_EQ(sum[1], 5.0f);
  EXPECT_FLOAT_EQ(sum[2], 5.0f);
  EXPECT_FLOAT_EQ(sum[3], 5.0f);

  const std::array<float, 4> product = (lhs * rhs).toArray();
  EXPECT_FLOAT_EQ(product[0], 4.0f);
  EXPECT_FLOAT_EQ(product[1], 6.0f);
  EXPECT_FLOAT_EQ(product[2], 6.0f);
  EXPECT_FLOAT_EQ(product[3], 4.0f);
}

TEST(WideTest, AverageFromColors) {
  F32x4 accum = F32x4::Splat(0.0f);
  accum += F32x4::FromColor(Color::RGB(10, 20, 30));
  accum += F32x4::FromColor(Color(20, 40, 60, 128));

  const std::array<float, 4> averaged = (accum / 2.0f).toArray();
  EXPECT_FLOAT_EQ(averaged[0], 15.0f);
  EXPECT_FLOAT_EQ(averaged[1], 30.0f);
  EXPECT_FLOAT_EQ(averaged[2], 45.0f);
  EXPECT_FLOAT_EQ(averaged[3], 191.5f);
}

}  // namespace donner::backends::tiny_skia_cpp

