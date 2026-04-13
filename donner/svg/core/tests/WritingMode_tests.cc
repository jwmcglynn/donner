/**
 * Tests for \ref donner::svg::WritingMode enum, its ostream output operator, and
 * \ref donner::svg::isVertical.
 */

#include "donner/svg/core/WritingMode.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref WritingMode values.
TEST(WritingModeTest, OstreamOutput) {
  EXPECT_THAT(WritingMode::HorizontalTb, ToStringIs("horizontal-tb"));
  EXPECT_THAT(WritingMode::VerticalRl, ToStringIs("vertical-rl"));
  EXPECT_THAT(WritingMode::VerticalLr, ToStringIs("vertical-lr"));
}

/// @test \ref isVertical returns correct results.
TEST(WritingModeTest, IsVertical) {
  EXPECT_FALSE(isVertical(WritingMode::HorizontalTb));
  EXPECT_TRUE(isVertical(WritingMode::VerticalRl));
  EXPECT_TRUE(isVertical(WritingMode::VerticalLr));
}

}  // namespace donner::svg
