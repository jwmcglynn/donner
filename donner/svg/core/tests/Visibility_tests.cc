/**
 * @file Tests for \ref donner::svg::Visibility enum and its ostream output operator.
 */

#include "donner/svg/core/Visibility.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref Visibility values.
TEST(VisibilityTest, OstreamOutput) {
  EXPECT_THAT(Visibility::Visible, ToStringIs("visible"));
  EXPECT_THAT(Visibility::Hidden, ToStringIs("hidden"));
  EXPECT_THAT(Visibility::Collapse, ToStringIs("collapse"));
}

}  // namespace donner::svg
