/**
 * @file Tests for \ref donner::svg::ClipRule enum and its ostream output operator.
 */

#include "donner/svg/core/ClipRule.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref ClipRule values.
TEST(ClipRuleTest, Output) {
  EXPECT_THAT(ClipRule::NonZero, ToStringIs("nonzero"));
  EXPECT_THAT(ClipRule::EvenOdd, ToStringIs("evenodd"));
}

}  // namespace donner::svg
