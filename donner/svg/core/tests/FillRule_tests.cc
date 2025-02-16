/**
 * @file Tests for \ref donner::svg::FillRule enum and its ostream output operator.
 */

#include "donner/svg/core/FillRule.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref FillRule values.
TEST(FillRuleTest, OstreamOutput) {
  EXPECT_THAT(FillRule::NonZero, ToStringIs("nonzero"));
  EXPECT_THAT(FillRule::EvenOdd, ToStringIs("evenodd"));
}

}  // namespace donner::svg
