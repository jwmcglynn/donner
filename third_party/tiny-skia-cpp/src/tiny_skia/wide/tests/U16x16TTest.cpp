#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/wide/U16x16T.h"

namespace {

using ::testing::ElementsAreArray;
using tiny_skia::wide::U16x16T;

TEST(U16x16TTest, MinMaxCmpLeAndBlendArePerLane) {
  const std::array<std::uint16_t, 16> lhsLanes = {1, 9, 100, 65535, 8,   7,    6,    5,
                                                  4, 3, 2,   1,     500, 1000, 1500, 2000};
  const std::array<std::uint16_t, 16> rhsLanes = {2, 5, 100, 0, 9,   1,    7,    5,
                                                  1, 4, 2,   9, 400, 1200, 1500, 1999};
  const U16x16T lhs(lhsLanes);
  const U16x16T rhs(rhsLanes);

  std::array<std::uint16_t, 16> expectedMin{};
  std::array<std::uint16_t, 16> expectedMax{};
  std::array<std::uint16_t, 16> expectedCmpLe{};
  for (std::size_t i = 0; i < expectedMin.size(); ++i) {
    expectedMin[i] = std::min(lhsLanes[i], rhsLanes[i]);
    expectedMax[i] = std::max(lhsLanes[i], rhsLanes[i]);
    expectedCmpLe[i] = lhsLanes[i] <= rhsLanes[i] ? UINT16_MAX : 0;
  }

  EXPECT_THAT(lhs.min(rhs).lanes(), ElementsAreArray(expectedMin));
  EXPECT_THAT(lhs.max(rhs).lanes(), ElementsAreArray(expectedMax));
  EXPECT_THAT(lhs.cmpLe(rhs).lanes(), ElementsAreArray(expectedCmpLe));

  const std::array<std::uint16_t, 16> onTrue = {10, 11, 12, 13, 14, 15, 16, 17,
                                                18, 19, 20, 21, 22, 23, 24, 25};
  const std::array<std::uint16_t, 16> onFalse = {100, 101, 102, 103, 104, 105, 106, 107,
                                                 108, 109, 110, 111, 112, 113, 114, 115};
  const U16x16T mask(expectedCmpLe);
  std::array<std::uint16_t, 16> expectedBlend{};
  for (std::size_t i = 0; i < expectedBlend.size(); ++i) {
    expectedBlend[i] = static_cast<std::uint16_t>((onTrue[i] & expectedCmpLe[i]) |
                                                  (onFalse[i] & ~expectedCmpLe[i]));
  }

  EXPECT_THAT(mask.blend(U16x16T(onTrue), U16x16T(onFalse)).lanes(),
              ElementsAreArray(expectedBlend));
}

TEST(U16x16TTest, ArithmeticOpsMatchPerLane) {
  const std::array<std::uint16_t, 16> lhsLanes = {65535, 100, 200,  300,  400,  500,  600,  700,
                                                  800,   900, 1000, 1100, 1200, 1300, 1400, 1500};
  const std::array<std::uint16_t, 16> rhsLanes = {1, 2,  3,  4,  5,  6,  7,  8,
                                                  9, 10, 11, 12, 13, 14, 15, 16};
  const U16x16T lhs(lhsLanes);
  const U16x16T rhs(rhsLanes);

  std::array<std::uint16_t, 16> expectedAdd{};
  std::array<std::uint16_t, 16> expectedSub{};
  std::array<std::uint16_t, 16> expectedMul{};
  std::array<std::uint16_t, 16> expectedDiv{};
  for (std::size_t i = 0; i < expectedAdd.size(); ++i) {
    expectedAdd[i] = static_cast<std::uint16_t>(lhsLanes[i] + rhsLanes[i]);
    expectedSub[i] = static_cast<std::uint16_t>(lhsLanes[i] - rhsLanes[i]);
    expectedMul[i] = static_cast<std::uint16_t>(lhsLanes[i] * rhsLanes[i]);
    expectedDiv[i] = static_cast<std::uint16_t>(lhsLanes[i] / rhsLanes[i]);
  }

  EXPECT_THAT((lhs + rhs).lanes(), ElementsAreArray(expectedAdd));
  EXPECT_THAT((lhs - rhs).lanes(), ElementsAreArray(expectedSub));
  EXPECT_THAT((lhs * rhs).lanes(), ElementsAreArray(expectedMul));
  EXPECT_THAT((lhs / rhs).lanes(), ElementsAreArray(expectedDiv));
}

TEST(U16x16TTest, BitwiseAndShiftOpsMatchPerLane) {
  const std::array<std::uint16_t, 16> lhsLanes = {0xFFFF, 0x00FF, 0x0F0F, 0x3333, 1, 2,  3,  4,
                                                  5,      6,      7,      8,      9, 10, 11, 12};
  const std::array<std::uint16_t, 16> rhsLanes = {1, 0x0F0F, 0x00FF, 0x5555, 2,  3,  4,  5,
                                                  6, 7,      8,      9,      10, 11, 12, 13};
  const U16x16T lhs(lhsLanes);
  const U16x16T rhs(rhsLanes);

  std::array<std::uint16_t, 16> expectedAnd{};
  std::array<std::uint16_t, 16> expectedOr{};
  std::array<std::uint16_t, 16> expectedNot{};
  for (std::size_t i = 0; i < expectedAnd.size(); ++i) {
    expectedAnd[i] = static_cast<std::uint16_t>(lhsLanes[i] & rhsLanes[i]);
    expectedOr[i] = static_cast<std::uint16_t>(lhsLanes[i] | rhsLanes[i]);
    expectedNot[i] = static_cast<std::uint16_t>(~lhsLanes[i]);
  }

  EXPECT_THAT((lhs & rhs).lanes(), ElementsAreArray(expectedAnd));
  EXPECT_THAT((lhs | rhs).lanes(), ElementsAreArray(expectedOr));
  EXPECT_THAT((~lhs).lanes(), ElementsAreArray(expectedNot));

  const std::array<std::uint16_t, 16> shiftLanes = {0, 1, 1, 2, 2, 3, 3, 4, 0, 1, 1, 2, 2, 3, 3, 4};
  const U16x16T shifts(shiftLanes);
  std::array<std::uint16_t, 16> expectedShr{};
  for (std::size_t i = 0; i < expectedShr.size(); ++i) {
    expectedShr[i] = static_cast<std::uint16_t>(lhsLanes[i] >> shiftLanes[i]);
  }

  EXPECT_THAT((lhs >> shifts).lanes(), ElementsAreArray(expectedShr));
}

}  // namespace
