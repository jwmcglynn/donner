/**
 * @file Tests for \ref donner::svg::ClipPathUnits enum and its ostream output operator.
 */

#include "donner/svg/core/ClipPathUnits.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref ClipPathUnits values.
TEST(ClipPathUnitsTest, Output) {
  EXPECT_THAT(ClipPathUnits::UserSpaceOnUse, ToStringIs("userSpaceOnUse"));
  EXPECT_THAT(ClipPathUnits::ObjectBoundingBox, ToStringIs("objectBoundingBox"));
  EXPECT_THAT(ClipPathUnits::Default, ToStringIs("userSpaceOnUse"));
}

}  // namespace donner::svg
