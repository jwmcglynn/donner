#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "tiny_skia/Math.h"

namespace {

constexpr float kFloatTolerance = 0.0001f;

}  // namespace

TEST(MathTest, BoundClampsToRange) {
  const std::array<unsigned, 3> results{
      tiny_skia::bound(0u, 3u, 10u),
      tiny_skia::bound(5u, 3u, 10u),
      tiny_skia::bound(0u, 13u, 10u),
  };
  EXPECT_THAT(results, testing::ElementsAre(3u, 5u, 10u));
}

TEST(MathTest, LeftShiftAndApproxPowf) {
  const std::array<std::int32_t, 3> leftShiftResults{
      tiny_skia::leftShift(3, 4),
      tiny_skia::leftShift(-1, 31),
      tiny_skia::leftShift(-1, 0),
  };
  EXPECT_THAT(leftShiftResults,
              testing::ElementsAre(48, std::numeric_limits<std::int32_t>::min(), -1));

  const std::array<std::int64_t, 2> leftShift64Results{
      tiny_skia::leftShift64(-1, 1),
      tiny_skia::leftShift64(1, 63),
  };
  EXPECT_THAT(leftShift64Results,
              testing::ElementsAre(static_cast<std::int64_t>(0xFFFFFFFFFFFFFFFEULL),
                                   std::numeric_limits<std::int64_t>::min()));

  EXPECT_FLOAT_EQ(tiny_skia::approxPowf(0.0f, 3.0f), 0.0f);
  EXPECT_FLOAT_EQ(tiny_skia::approxPowf(1.0f, 3.0f), 1.0f);

  EXPECT_NEAR(tiny_skia::approxPowf(2.0f, 0.5f), 1.4142135f, 0.02f);
  EXPECT_NEAR(tiny_skia::approxPowf(4.0f, 0.5f), 2.0f, 0.02f);
  EXPECT_NEAR(tiny_skia::approxPowf(4.0f, 0.0f), 1.0f, kFloatTolerance);
  EXPECT_NEAR(tiny_skia::approxPowf(9.0f, 0.5f), 3.0f, 0.02f);
  EXPECT_NEAR(tiny_skia::approxPowf(2.0f, 1.0f), 2.0f, kFloatTolerance);
  EXPECT_NEAR(tiny_skia::approxPowf(4.0f, 1.0f), 4.0f, kFloatTolerance);
  EXPECT_NEAR(tiny_skia::approxPowf(16.0f, 0.5f), 4.0f, 0.03f);
  EXPECT_NEAR(tiny_skia::approxPowf(4.0f, 0.25f), 1.4142f, 0.03f);
}
