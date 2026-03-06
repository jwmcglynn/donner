#include <array>
#include <bit>
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/wide/F32x4T.h"
#include "tiny_skia/wide/I32x4T.h"

namespace {

using ::testing::ElementsAre;
using tiny_skia::wide::I32x4T;

TEST(I32x4TTest, ComparisonsEmitMinusOneMasks) {
  const I32x4T lhs({1, 5, 7, 9});
  const I32x4T rhs({1, 3, 7, 10});

  EXPECT_THAT(lhs.cmpEq(rhs).lanes(), ElementsAre(-1, 0, -1, 0));
  EXPECT_THAT(lhs.cmpGt(rhs).lanes(), ElementsAre(0, -1, 0, 0));
  EXPECT_THAT(lhs.cmpLt(rhs).lanes(), ElementsAre(0, 0, 0, -1));
}

TEST(I32x4TTest, BlendUsesMaskBitsPerLane) {
  const I32x4T mask({-1, 0, -1, 0});
  const I32x4T onTrue({10, 20, 30, 40});
  const I32x4T onFalse({1, 2, 3, 4});

  EXPECT_THAT(mask.blend(onTrue, onFalse).lanes(), ElementsAre(10, 2, 30, 4));
}

TEST(I32x4TTest, AddAndMulUseWrappingSemantics) {
  const I32x4T lhs({0x7FFFFFFF, -1, 46341, 100000});
  const I32x4T rhs({1, 2, 46341, 30000});

  EXPECT_THAT((lhs + rhs).lanes(), ElementsAre(-2147483648, 1, 92682, 130000));
  EXPECT_THAT((lhs * rhs).lanes(), ElementsAre(2147483647, -2, -2147479015, -1294967296));
}

TEST(I32x4TTest, ToF32x4AndBitcastMatchRustConversions) {
  const I32x4T value({1, -2, 3, -4});

  const tiny_skia::wide::F32x4T numeric = value.toF32x4();
  EXPECT_THAT(numeric.lanes(), ElementsAre(1.0f, -2.0f, 3.0f, -4.0f));

  const tiny_skia::wide::F32x4T bitcast = value.toF32x4Bitcast();
  EXPECT_EQ(std::bit_cast<std::int32_t>(bitcast.lanes()[0]), 1);
  EXPECT_EQ(std::bit_cast<std::int32_t>(bitcast.lanes()[1]), -2);
}

}  // namespace
