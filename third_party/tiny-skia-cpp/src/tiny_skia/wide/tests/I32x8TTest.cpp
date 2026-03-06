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
using tiny_skia::wide::I32x8T;

TEST(I32x8TTest, ComparisonsEmitMinusOneMasks) {
  const I32x8T lhs({1, 5, 7, 9, 10, -3, 6, 8});
  const I32x8T rhs({1, 3, 7, 10, 2, -3, 9, 0});

  EXPECT_THAT(lhs.cmpEq(rhs).lanes(), ElementsAre(-1, 0, -1, 0, 0, -1, 0, 0));
  EXPECT_THAT(lhs.cmpGt(rhs).lanes(), ElementsAre(0, -1, 0, 0, -1, 0, 0, -1));
  EXPECT_THAT(lhs.cmpLt(rhs).lanes(), ElementsAre(0, 0, 0, -1, 0, 0, -1, 0));
}

TEST(I32x8TTest, BlendUsesMaskBitsPerLane) {
  const I32x8T mask({-1, 0, -1, 0, -1, 0, -1, 0});
  const I32x8T onTrue({10, 20, 30, 40, 50, 60, 70, 80});
  const I32x8T onFalse({1, 2, 3, 4, 5, 6, 7, 8});

  EXPECT_THAT(mask.blend(onTrue, onFalse).lanes(), ElementsAre(10, 2, 30, 4, 50, 6, 70, 8));
}

TEST(I32x8TTest, AddAndMulUseWrappingSemantics) {
  const I32x8T lhs({0x7FFFFFFF, -1, 46341, 100000, INT32_MIN, 123, 456, 789});
  const I32x8T rhs({1, 2, 46341, 30000, -1, 1000, -1, -2});

  EXPECT_THAT((lhs + rhs).lanes(),
              ElementsAre(-2147483648, 1, 92682, 130000, 2147483647, 1123, 455, 787));
  EXPECT_THAT((lhs * rhs).lanes(), ElementsAre(2147483647, -2, -2147479015, -1294967296,
                                               -2147483648, 123000, -456, -1578));
}

TEST(I32x8TTest, ToF32x8AndBitcastMatchRustConversions) {
  const I32x8T value({1, -2, 3, -4, 5, -6, 7, -8});

  const tiny_skia::wide::F32x8T numeric = value.toF32x8();
  EXPECT_THAT(numeric.lanes(), ElementsAre(1.0f, -2.0f, 3.0f, -4.0f, 5.0f, -6.0f, 7.0f, -8.0f));

  const tiny_skia::wide::F32x8T bitcast = value.toF32x8Bitcast();
  EXPECT_EQ(std::bit_cast<std::int32_t>(bitcast.lanes()[0]), 1);
  EXPECT_EQ(std::bit_cast<std::int32_t>(bitcast.lanes()[1]), -2);
  EXPECT_EQ(std::bit_cast<std::int32_t>(bitcast.lanes()[7]), -8);

  const auto asU32 = value.toU32x8Bitcast().lanes();
  EXPECT_THAT(asU32,
              ElementsAre(std::bit_cast<std::uint32_t>(1), std::bit_cast<std::uint32_t>(-2),
                          std::bit_cast<std::uint32_t>(3), std::bit_cast<std::uint32_t>(-4),
                          std::bit_cast<std::uint32_t>(5), std::bit_cast<std::uint32_t>(-6),
                          std::bit_cast<std::uint32_t>(7), std::bit_cast<std::uint32_t>(-8)));
}

}  // namespace
