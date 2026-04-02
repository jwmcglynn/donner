#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/wide/F32x8T.h"
#include "tiny_skia/wide/I32x8T.h"
#include "tiny_skia/wide/U32x8T.h"

namespace {

using ::testing::ElementsAre;
using tiny_skia::wide::F32x8T;

TEST(F32x8TTest, AbsMinMaxAndNormalizeArePerLane) {
  const F32x8T value({-1.5f, 2.0f, -0.0f, -99.25f, 3.0f, -4.0f, 5.5f, 2.5f});
  EXPECT_THAT(value.abs().lanes(), ElementsAre(1.5f, 2.0f, 0.0f, 99.25f, 3.0f, 4.0f, 5.5f, 2.5f));

  const F32x8T lhs({1.0f, -3.0f, 5.0f, 4.0f, -10.0f, 6.0f, 7.0f, 8.0f});
  const F32x8T rhs({2.0f, -4.0f, 3.0f, 10.0f, 1.0f, 5.0f, 9.0f, -8.0f});
  EXPECT_THAT(lhs.min(rhs).lanes(),
              ElementsAre(1.0f, -4.0f, 3.0f, 4.0f, -10.0f, 5.0f, 7.0f, -8.0f));
  EXPECT_THAT(lhs.max(rhs).lanes(), ElementsAre(2.0f, -3.0f, 5.0f, 10.0f, 1.0f, 6.0f, 9.0f, 8.0f));

  EXPECT_THAT(F32x8T({-1.0f, 0.2f, 1.5f, 2.0f, -0.3f, 0.7f, 1.0f, 0.0f}).normalize().lanes(),
              ElementsAre(0.0f, 0.2f, 1.0f, 1.0f, 0.0f, 0.7f, 1.0f, 0.0f));
}

TEST(F32x8TTest, ComparisonsAndBlendUseMaskBits) {
  const F32x8T lhs({1.0f, 2.0f, 3.0f, 4.0f, 4.0f, 6.0f, 7.0f, 8.0f});
  const F32x8T rhs({1.0f, 0.0f, 5.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f});

  const auto eq = lhs.cmpEq(rhs).lanes();
  EXPECT_EQ(std::bit_cast<std::uint32_t>(eq[0]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(eq[1]), 0u);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(eq[3]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(eq[7]), 0u);

  const F32x8T mask({std::bit_cast<float>(0xFFFFFFFFu), 0.0f, std::bit_cast<float>(0xFFFFFFFFu),
                     0.0f, std::bit_cast<float>(0xFFFFFFFFu), 0.0f,
                     std::bit_cast<float>(0xFFFFFFFFu), 0.0f});
  const F32x8T onTrue({10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f});
  const F32x8T onFalse({1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f});
  EXPECT_THAT(mask.blend(onTrue, onFalse).lanes(),
              ElementsAre(10.0f, 2.0f, 30.0f, 4.0f, 50.0f, 6.0f, 70.0f, 8.0f));
}

TEST(F32x8TTest, FloorFractRoundAndIntegerConversionsMatchScalarFallback) {
  const F32x8T value({1.9f, -1.1f, 2.5f, -2.5f, 0.0f, 3.99f, -3.99f, 8.01f});

  EXPECT_THAT(value.floor().lanes(),
              ElementsAre(1.0f, -2.0f, 2.0f, -3.0f, 0.0f, 3.0f, -4.0f, 8.0f));

  const auto fract = value.fract().lanes();
  EXPECT_FLOAT_EQ(fract[0], 0.9f);
  EXPECT_FLOAT_EQ(fract[1], 0.9f);
  EXPECT_FLOAT_EQ(fract[2], 0.5f);
  EXPECT_FLOAT_EQ(fract[3], 0.5f);

  EXPECT_THAT(value.round().lanes(),
              ElementsAre(2.0f, -1.0f, 2.0f, -2.0f, 0.0f, 4.0f, -4.0f, 8.0f));

  const tiny_skia::wide::I32x8T rounded = value.roundInt();
  EXPECT_THAT(rounded.lanes(), ElementsAre(2, -1, 2, -2, 0, 4, -4, 8));

  const tiny_skia::wide::I32x8T truncated = value.truncInt();
  EXPECT_THAT(truncated.lanes(), ElementsAre(1, -1, 2, -2, 0, 3, -3, 8));
}

TEST(F32x8TTest, BitcastConversionsPreserveAllLaneBits) {
  const F32x8T value({std::bit_cast<float>(0x3f800000u), std::bit_cast<float>(0xbf800000u),
                      std::bit_cast<float>(0x00000000u), std::bit_cast<float>(0x80000000u),
                      std::bit_cast<float>(0x7f800000u), std::bit_cast<float>(0xff800000u),
                      std::bit_cast<float>(0x7fc00000u), std::bit_cast<float>(0x12345678u)});

  const auto asI32 = value.toI32x8Bitcast().lanes();
  EXPECT_THAT(
      asI32,
      ElementsAre(
          std::bit_cast<std::int32_t>(0x3f800000u), std::bit_cast<std::int32_t>(0xbf800000u),
          std::bit_cast<std::int32_t>(0x00000000u), std::bit_cast<std::int32_t>(0x80000000u),
          std::bit_cast<std::int32_t>(0x7f800000u), std::bit_cast<std::int32_t>(0xff800000u),
          std::bit_cast<std::int32_t>(0x7fc00000u), std::bit_cast<std::int32_t>(0x12345678u)));

  const auto asU32 = value.toU32x8Bitcast().lanes();
  EXPECT_THAT(asU32, ElementsAre(0x3f800000u, 0xbf800000u, 0x00000000u, 0x80000000u, 0x7f800000u,
                                 0xff800000u, 0x7fc00000u, 0x12345678u));
}

TEST(F32x8TTest, IsFiniteMatchesRustBitMaskLogic) {
  const F32x8T value({0.0f, std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN(), 1.0f, -42.0f, 3.14f, -0.0f});

  const auto mask = value.isFinite().lanes();
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[0]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[1]), 0u);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[2]), 0u);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[3]), 0u);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[4]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[5]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[6]), 0xFFFFFFFFu);
  EXPECT_EQ(std::bit_cast<std::uint32_t>(mask[7]), 0xFFFFFFFFu);
}

TEST(F32x8TTest, SqrtIsPerLane) {
  const F32x8T value({0.0f, 1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 0.25f, 100.0f});
  EXPECT_THAT(value.sqrt().lanes(), ElementsAre(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 0.5f, 10.0f));
}

TEST(F32x8TTest, RecipFastIsPerLane) {
  const F32x8T value({1.0f, 2.0f, 4.0f, 0.5f, 8.0f, 0.25f, 10.0f, 0.1f});
  const auto result = value.recipFast().lanes();
  constexpr float kTolerance = 2e-5f;
  EXPECT_NEAR(result[0], 1.0f, kTolerance);
  EXPECT_NEAR(result[1], 0.5f, kTolerance);
  EXPECT_NEAR(result[2], 0.25f, kTolerance);
  EXPECT_NEAR(result[3], 2.0f, kTolerance);
  EXPECT_NEAR(result[4], 0.125f, kTolerance);
  EXPECT_NEAR(result[5], 4.0f, kTolerance);
  EXPECT_NEAR(result[6], 0.1f, kTolerance);
  EXPECT_NEAR(result[7], 10.0f, kTolerance);
}

TEST(F32x8TTest, RecipSqrtIsPerLane) {
  const F32x8T value({1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 100.0f, 0.25f, 0.01f});
  const auto result = value.recipSqrt().lanes();
  constexpr float kTolerance = 2e-5f;
  EXPECT_NEAR(result[0], 1.0f, kTolerance);
  EXPECT_NEAR(result[1], 0.5f, kTolerance);
  EXPECT_NEAR(result[2], 1.0f / 3.0f, kTolerance);
  EXPECT_NEAR(result[3], 0.25f, kTolerance);
  EXPECT_NEAR(result[4], 0.2f, kTolerance);
  EXPECT_NEAR(result[5], 0.1f, kTolerance);
  EXPECT_NEAR(result[6], 2.0f, kTolerance);
  EXPECT_NEAR(result[7], 10.0f, kTolerance);
}

TEST(F32x8TTest, PowfIsPerLane) {
  const F32x8T base({1.0f, 2.0f, 3.0f, 4.0f, 9.0f, 8.0f, 27.0f, 16.0f});
  const auto squared = base.powf(2.0f).lanes();
  EXPECT_FLOAT_EQ(squared[0], 1.0f);
  EXPECT_FLOAT_EQ(squared[1], 4.0f);
  EXPECT_FLOAT_EQ(squared[2], 9.0f);
  EXPECT_FLOAT_EQ(squared[3], 16.0f);
  EXPECT_FLOAT_EQ(squared[4], 81.0f);
  EXPECT_FLOAT_EQ(squared[5], 64.0f);
  EXPECT_FLOAT_EQ(squared[6], 729.0f);
  EXPECT_FLOAT_EQ(squared[7], 256.0f);

  const auto sqrtResult = base.powf(0.5f).lanes();
  EXPECT_FLOAT_EQ(sqrtResult[0], 1.0f);
  EXPECT_FLOAT_EQ(sqrtResult[3], 2.0f);
  EXPECT_FLOAT_EQ(sqrtResult[4], 3.0f);
  EXPECT_FLOAT_EQ(sqrtResult[7], 4.0f);
}

TEST(F32x8TTest, OperatorPlusEqualsIsPerLane) {
  F32x8T value({1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f});
  const F32x8T addend({10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f});
  value += addend;
  EXPECT_THAT(value.lanes(), ElementsAre(11.0f, 22.0f, 33.0f, 44.0f, 55.0f, 66.0f, 77.0f, 88.0f));
}

}  // namespace
