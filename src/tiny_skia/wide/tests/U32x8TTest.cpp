#include <array>
#include <bit>
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/wide/F32x8T.h"
#include "tiny_skia/wide/I32x8T.h"
#include "tiny_skia/wide/U32x8T.h"

namespace {

using ::testing::ElementsAre;
using tiny_skia::wide::U32x8T;

TEST(U32x8TTest, CmpEqProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 2, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({1, 9, 3, 0, 5, 0, 0, 8});

  EXPECT_THAT(lhs.cmpEq(rhs).lanes(),
              ElementsAre(UINT32_MAX, 0u, UINT32_MAX, 0u, UINT32_MAX, 0u, 0u, UINT32_MAX));
}

TEST(U32x8TTest, BitcastConversionsPreserveAllLaneBits) {
  const U32x8T value({0x3f800000u, 0xbf800000u, 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
                      0x7fc00000u, 0x12345678u});

  const auto asI32 = value.toI32x8Bitcast().lanes();
  EXPECT_THAT(
      asI32,
      ElementsAre(
          std::bit_cast<std::int32_t>(0x3f800000u), std::bit_cast<std::int32_t>(0xbf800000u),
          std::bit_cast<std::int32_t>(0x00000000u), std::bit_cast<std::int32_t>(0x80000000u),
          std::bit_cast<std::int32_t>(0x7f800000u), std::bit_cast<std::int32_t>(0xff800000u),
          std::bit_cast<std::int32_t>(0x7fc00000u), std::bit_cast<std::int32_t>(0x12345678u)));

  const auto asF32 = value.toF32x8Bitcast().lanes();
  const std::array<std::uint32_t, 8> roundTripBits = {
      std::bit_cast<std::uint32_t>(asF32[0]), std::bit_cast<std::uint32_t>(asF32[1]),
      std::bit_cast<std::uint32_t>(asF32[2]), std::bit_cast<std::uint32_t>(asF32[3]),
      std::bit_cast<std::uint32_t>(asF32[4]), std::bit_cast<std::uint32_t>(asF32[5]),
      std::bit_cast<std::uint32_t>(asF32[6]), std::bit_cast<std::uint32_t>(asF32[7]),
  };
  EXPECT_THAT(roundTripBits, ElementsAre(0x3f800000u, 0xbf800000u, 0x00000000u, 0x80000000u,
                                         0x7f800000u, 0xff800000u, 0x7fc00000u, 0x12345678u));
}

TEST(U32x8TTest, ShiftOperationsMatchRustScalarFallback) {
  const U32x8T value({1, 2, 4, 8, 16, 32, 64, 128});
  EXPECT_THAT(value.shl<2>().lanes(), ElementsAre(4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u));
  EXPECT_THAT(value.shr<1>().lanes(), ElementsAre(0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u));
}

TEST(U32x8TTest, BitwiseAndAddOpsArePerLane) {
  const U32x8T lhs({0xFFFFFFFFu, 0x0F0F0F0Fu, 1u, 10u, 100u, 1000u, 10000u, 100000u});
  const U32x8T rhs({1u, 0xF0F0F0F0u, 2u, 20u, 200u, 2000u, 20000u, 200000u});

  EXPECT_THAT((~lhs).lanes(), ElementsAre(0u, 0xF0F0F0F0u, 0xFFFFFFFEu, 0xFFFFFFF5u, 0xFFFFFF9Bu,
                                          0xFFFFFC17u, 0xFFFFD8EFu, 0xFFFE795Fu));
  EXPECT_THAT((lhs + rhs).lanes(),
              ElementsAre(0u, 0xFFFFFFFFu, 3u, 30u, 300u, 3000u, 30000u, 300000u));
  EXPECT_THAT((lhs & rhs).lanes(), ElementsAre(1u, 0u, 0u, 0u, 64u, 960u, 1536u, 66560u));
  EXPECT_THAT((lhs | rhs).lanes(),
              ElementsAre(0xFFFFFFFFu, 0xFFFFFFFFu, 3u, 30u, 236u, 2040u, 28464u, 233440u));
}

TEST(U32x8TTest, BitwiseXorIsPerLane) {
  const U32x8T lhs({0xFFFFFFFFu, 0x0F0F0F0Fu, 1u, 10u, 100u, 1000u, 10000u, 100000u});
  const U32x8T rhs({1u, 0xF0F0F0F0u, 2u, 20u, 200u, 2000u, 20000u, 200000u});

  EXPECT_THAT((lhs ^ rhs).lanes(),
              ElementsAre(0xFFFFFFFEu, 0xFFFFFFFFu, 3u, 30u, 172u, 1080u, 26928u, 166880u));
}

TEST(U32x8TTest, CmpNeProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 2, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({1, 9, 3, 0, 5, 0, 0, 8});

  EXPECT_THAT(lhs.cmpNe(rhs).lanes(),
              ElementsAre(0u, UINT32_MAX, 0u, UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX, 0u));
}

TEST(U32x8TTest, CmpLtProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 9, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({2, 5, 3, 10, 5, 0, 100, 8});

  EXPECT_THAT(lhs.cmpLt(rhs).lanes(),
              ElementsAre(UINT32_MAX, 0u, 0u, UINT32_MAX, 0u, 0u, UINT32_MAX, 0u));
}

TEST(U32x8TTest, CmpLeProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 9, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({2, 5, 3, 10, 5, 0, 100, 8});

  EXPECT_THAT(lhs.cmpLe(rhs).lanes(), ElementsAre(UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX,
                                                  UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX));
}

TEST(U32x8TTest, CmpGtProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 9, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({2, 5, 3, 10, 5, 0, 100, 8});

  EXPECT_THAT(lhs.cmpGt(rhs).lanes(), ElementsAre(0u, UINT32_MAX, 0u, 0u, 0u, UINT32_MAX, 0u, 0u));
}

TEST(U32x8TTest, CmpGeProducesAllOnesMaskPerLane) {
  const U32x8T lhs({1, 9, 3, 4, 5, 6, 7, 8});
  const U32x8T rhs({2, 5, 3, 10, 5, 0, 100, 8});

  EXPECT_THAT(lhs.cmpGe(rhs).lanes(),
              ElementsAre(0u, UINT32_MAX, UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX, 0u, UINT32_MAX));
}

}  // namespace
