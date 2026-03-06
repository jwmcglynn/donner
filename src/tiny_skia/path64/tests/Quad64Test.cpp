#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "tiny_skia/path64/Quad64.h"

TEST(Quad64Test, PushValidTsFiltersAndDedups) {
  const std::array<double, 3> roots{-1.0, 0.2, 0.2};
  std::array<double, 3> filtered{};
  const auto count = tiny_skia::path64::quad64::pushValidTs(roots, 3, filtered);
  EXPECT_EQ(count, 1);
  EXPECT_THAT((std::array<double, 1>{filtered[0]}), testing::ElementsAre(testing::DoubleEq(0.2)));
}

TEST(Quad64Test, RootsRealFromMonicQuadratic) {
  std::array<double, 3> roots{};
  const auto count = tiny_skia::path64::quad64::rootsReal(1.0, -3.0, 2.0, roots);
  EXPECT_EQ(count, 2);
  EXPECT_THAT(
      (std::array<double, 2>{roots[0], roots[1]}),
      testing::ElementsAre(testing::DoubleNear(2.0, 1e-12), testing::DoubleNear(1.0, 1e-12)));
}

TEST(Quad64Test, RootsValidTClampsWithinUnitInterval) {
  std::array<double, 3> roots{};
  const auto count = tiny_skia::path64::quad64::rootsValidT(-1.0, 1.0, 0.0, roots);
  EXPECT_EQ(count, 2);
  EXPECT_THAT((std::array<double, 2>{roots[0], roots[1]}),
              testing::UnorderedElementsAre(testing::DoubleEq(0.0), testing::DoubleEq(1.0)));
}
