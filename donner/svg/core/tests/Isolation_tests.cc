/**
 * @file Tests for \ref donner::svg::Isolation enum and its ostream output operator.
 */

#include "donner/svg/core/Isolation.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref Isolation values.
TEST(IsolationTest, OstreamOutput) {
  EXPECT_THAT(Isolation::Auto, ToStringIs("auto"));
  EXPECT_THAT(Isolation::Isolate, ToStringIs("isolate"));
}

}  // namespace donner::svg
