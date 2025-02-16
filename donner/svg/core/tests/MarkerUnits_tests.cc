/**
 * @file Tests for \ref donner::svg::MarkerUnits enum and its ostream output operator.
 */

#include "donner/svg/core/MarkerUnits.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref MarkerUnits values.
TEST(MarkerUnitsTest, OstreamOutput) {
  EXPECT_THAT(MarkerUnits::StrokeWidth, ToStringIs("MarkerUnits::StrokeWidth"));
  EXPECT_THAT(MarkerUnits::UserSpaceOnUse, ToStringIs("MarkerUnits::UserSpaceOnUse"));
}

}  // namespace donner::svg
