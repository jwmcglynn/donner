#include "donner/svg/renderer/DecorationWorkBudget.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace donner::svg::detail {
namespace {

TEST(DecorationWorkBudget, RejectsSegmentCostsBeforeNarrowAccumulationCanWrap) {
  constexpr std::uint32_t kSegments = 1024;
  constexpr std::uint32_t kSegmentPoints = 2 + 4 * 1024 * 1024;
  constexpr std::uint32_t kAllowance = 1024 * 1024 - 4 * kSegments;
  std::uint32_t uncheckedTotal = 0;
  DecorationWorkBudget<std::uint32_t> budget(kAllowance);
  for (std::uint32_t index = 0; index < kSegments; ++index) {
    uncheckedTotal += kSegmentPoints;
    budget.add(kSegmentPoints);
  }
  EXPECT_EQ(uncheckedTotal, 2048u);
  EXPECT_LT(uncheckedTotal, kAllowance);
  EXPECT_TRUE(budget.exceeded());
  EXPECT_EQ(budget.remaining(), 0u);
  budget.add(0);
  EXPECT_TRUE(budget.exceeded());
  EXPECT_EQ(budget.remaining(), 0u);
}

TEST(DecorationWorkBudget, AcceptsMultipleSegmentsAtTheExactAllowance) {
  DecorationWorkBudget<std::uint32_t> budget(16);
  budget.add(8);
  budget.add(8);
  EXPECT_FALSE(budget.exceeded());
  EXPECT_EQ(budget.points(), 16u);
  EXPECT_EQ(budget.remaining(), 0u);
}

TEST(DecorationWorkBudget, ExhaustsOnAnAdditionalSegmentAfterTheExactAllowance) {
  DecorationWorkBudget<std::uint32_t> budget(16);
  budget.add(16);
  budget.add(1);
  EXPECT_TRUE(budget.exceeded());
  EXPECT_EQ(budget.remaining(), 0u);
}

}  // namespace
}  // namespace donner::svg::detail
