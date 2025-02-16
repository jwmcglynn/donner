/**
 * @file Tests for \ref donner::svg::Overflow enum and its ostream output operator.
 */

#include "donner/svg/core/Overflow.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref Overflow values.
TEST(OverflowTest, OstreamOutput) {
  EXPECT_THAT(Overflow::Visible, ToStringIs("visible"));
  EXPECT_THAT(Overflow::Hidden, ToStringIs("hidden"));
  EXPECT_THAT(Overflow::Scroll, ToStringIs("scroll"));
  EXPECT_THAT(Overflow::Auto, ToStringIs("auto"));
}

}  // namespace donner::svg
