#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "tiny_skia/FloatingPoint.h"

TEST(FloatingPointTest, NormalizedF32CreateAndClamp) {
  ASSERT_TRUE(tiny_skia::NormalizedF32::create(0.0f).has_value());
  ASSERT_TRUE(tiny_skia::NormalizedF32::create(1.0f).has_value());
  EXPECT_EQ(tiny_skia::NormalizedF32::create(-0.1f), std::nullopt);
  EXPECT_EQ(tiny_skia::NormalizedF32::create(1.1f), std::nullopt);
  EXPECT_EQ(tiny_skia::NormalizedF32::create(std::numeric_limits<float>::infinity()), std::nullopt);
  EXPECT_EQ(tiny_skia::NormalizedF32::create(std::numeric_limits<float>::quiet_NaN()),
            std::nullopt);

  EXPECT_FLOAT_EQ(tiny_skia::NormalizedF32::newClamped(-1.0f).get(), 0.0f);
  EXPECT_FLOAT_EQ(tiny_skia::NormalizedF32::newClamped(2.0f).get(), 1.0f);
  EXPECT_FLOAT_EQ(tiny_skia::NormalizedF32::fromU8(128).get(), 128.0f / 255.0f);
}

TEST(FloatingPointTest, NormalizedF32ExclusiveAndStrictWrappers) {
  ASSERT_TRUE(tiny_skia::NormalizedF32Exclusive::create(0.5f).has_value());
  EXPECT_EQ(tiny_skia::NormalizedF32Exclusive::create(0.0f), std::nullopt);
  EXPECT_EQ(tiny_skia::NormalizedF32Exclusive::create(1.0f), std::nullopt);
  EXPECT_NEAR(tiny_skia::NormalizedF32Exclusive::newBounded(-10.0f).get(),
              std::numeric_limits<float>::epsilon(), 0.0f);
  EXPECT_NEAR(tiny_skia::NormalizedF32Exclusive::newBounded(10.0f).get(),
              1.0f - std::numeric_limits<float>::epsilon(), 0.0f);

  ASSERT_TRUE(tiny_skia::NonZeroPositiveF32::create(1.0f).has_value());
  EXPECT_EQ(tiny_skia::NonZeroPositiveF32::create(0.0f), std::nullopt);
  EXPECT_EQ(tiny_skia::NonZeroPositiveF32::create(-1.0f), std::nullopt);
  EXPECT_EQ(tiny_skia::NonZeroPositiveF32::create(std::numeric_limits<float>::infinity()),
            std::nullopt);

  ASSERT_TRUE(tiny_skia::FiniteF32::create(0.0f).has_value());
  EXPECT_EQ(tiny_skia::FiniteF32::create(std::numeric_limits<float>::infinity()), std::nullopt);
  EXPECT_EQ(tiny_skia::FiniteF32::create(std::numeric_limits<float>::quiet_NaN()), std::nullopt);
}

TEST(FloatingPointTest, SaturatingCastsMatchRustSemantics) {
  constexpr std::int32_t kMaxI32FitsInF32 = 2147483520;
  constexpr std::int32_t kMinI32FitsInF32 = -2147483520;

  EXPECT_EQ(tiny_skia::saturateCastI32(1.9f), 1);
  EXPECT_EQ(tiny_skia::saturateCastI32(-1.9f), -1);
  EXPECT_EQ(tiny_skia::saturateCastI32(std::numeric_limits<float>::quiet_NaN()), kMaxI32FitsInF32);
  EXPECT_EQ(tiny_skia::saturateCastI32(std::numeric_limits<float>::infinity()), kMaxI32FitsInF32);
  EXPECT_EQ(tiny_skia::saturateCastI32(-std::numeric_limits<float>::infinity()), kMinI32FitsInF32);

  EXPECT_EQ(tiny_skia::saturateCastI32(std::numeric_limits<double>::quiet_NaN()),
            std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(tiny_skia::saturateCastI32(std::numeric_limits<double>::infinity()),
            std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(tiny_skia::saturateCastI32(-std::numeric_limits<double>::infinity()),
            std::numeric_limits<std::int32_t>::min());
}

TEST(FloatingPointTest, SaturatingRoundHelpersMatchTrait) {
  EXPECT_EQ(tiny_skia::saturateFloorToI32(1.2f), 1);
  EXPECT_EQ(tiny_skia::saturateCeilToI32(1.2f), 2);
  EXPECT_EQ(tiny_skia::saturateRoundToI32(1.2f), 1);
  EXPECT_EQ(tiny_skia::saturateRoundToI32(1.5f), 1);
  EXPECT_EQ(tiny_skia::saturateRoundToI32(-1.2f), -1);
  EXPECT_EQ(tiny_skia::saturateRoundToI32(std::numeric_limits<float>::quiet_NaN()), 2147483520);
}

TEST(FloatingPointTest, F32TwoSComplimentOrdering) {
  EXPECT_EQ(tiny_skia::f32As2sCompliment(-0.0f), tiny_skia::f32As2sCompliment(0.0f));
  EXPECT_LT(tiny_skia::f32As2sCompliment(-2.0f), tiny_skia::f32As2sCompliment(-1.0f));
  EXPECT_LT(tiny_skia::f32As2sCompliment(-1.0f), tiny_skia::f32As2sCompliment(0.0f));
  EXPECT_LT(tiny_skia::f32As2sCompliment(0.0f), tiny_skia::f32As2sCompliment(1.0f));
}
