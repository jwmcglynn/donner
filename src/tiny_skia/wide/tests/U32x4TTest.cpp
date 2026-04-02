#include <array>
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/wide/U32x4T.h"

namespace {

using ::testing::ElementsAre;
using tiny_skia::wide::U32x4T;

TEST(U32x4TTest, CmpEqProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 2, 3, 4});
  const U32x4T rhs({1, 9, 3, 0});

  EXPECT_THAT(lhs.cmpEq(rhs).lanes(), ElementsAre(UINT32_MAX, 0u, UINT32_MAX, 0u));
}

TEST(U32x4TTest, ShiftOperationsMatchRustScalarFallback) {
  const U32x4T value({1, 2, 4, 8});
  EXPECT_THAT(value.shl<2>().lanes(), ElementsAre(4u, 8u, 16u, 32u));
  EXPECT_THAT(value.shr<1>().lanes(), ElementsAre(0u, 1u, 2u, 4u));
}

TEST(U32x4TTest, BitwiseAndAddOpsArePerLane) {
  const U32x4T lhs({0xFFFFFFFFu, 0x0F0F0F0Fu, 1u, 10u});
  const U32x4T rhs({1u, 0xF0F0F0F0u, 2u, 20u});

  EXPECT_THAT((~lhs).lanes(), ElementsAre(0u, 0xF0F0F0F0u, 0xFFFFFFFEu, 0xFFFFFFF5u));
  EXPECT_THAT((lhs + rhs).lanes(), ElementsAre(0u, 0xFFFFFFFFu, 3u, 30u));
  EXPECT_THAT((lhs & rhs).lanes(), ElementsAre(1u, 0u, 0u, 0u));
  EXPECT_THAT((lhs | rhs).lanes(), ElementsAre(0xFFFFFFFFu, 0xFFFFFFFFu, 3u, 30u));
}

TEST(U32x4TTest, BitwiseXorIsPerLane) {
  const U32x4T lhs({0xFFFFFFFFu, 0x0F0F0F0Fu, 1u, 10u});
  const U32x4T rhs({1u, 0xF0F0F0F0u, 2u, 20u});

  EXPECT_THAT((lhs ^ rhs).lanes(), ElementsAre(0xFFFFFFFEu, 0xFFFFFFFFu, 3u, 30u));
}

TEST(U32x4TTest, CmpNeProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 2, 3, 4});
  const U32x4T rhs({1, 9, 3, 0});

  EXPECT_THAT(lhs.cmpNe(rhs).lanes(), ElementsAre(0u, UINT32_MAX, 0u, UINT32_MAX));
}

TEST(U32x4TTest, CmpLtProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 9, 3, 4});
  const U32x4T rhs({2, 5, 3, 10});

  EXPECT_THAT(lhs.cmpLt(rhs).lanes(), ElementsAre(UINT32_MAX, 0u, 0u, UINT32_MAX));
}

TEST(U32x4TTest, CmpLeProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 9, 3, 4});
  const U32x4T rhs({2, 5, 3, 10});

  EXPECT_THAT(lhs.cmpLe(rhs).lanes(), ElementsAre(UINT32_MAX, 0u, UINT32_MAX, UINT32_MAX));
}

TEST(U32x4TTest, CmpGtProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 9, 3, 4});
  const U32x4T rhs({2, 5, 3, 10});

  EXPECT_THAT(lhs.cmpGt(rhs).lanes(), ElementsAre(0u, UINT32_MAX, 0u, 0u));
}

TEST(U32x4TTest, CmpGeProducesAllOnesMaskPerLane) {
  const U32x4T lhs({1, 9, 3, 4});
  const U32x4T rhs({2, 5, 3, 10});

  EXPECT_THAT(lhs.cmpGe(rhs).lanes(), ElementsAre(0u, UINT32_MAX, UINT32_MAX, 0u));
}

}  // namespace
