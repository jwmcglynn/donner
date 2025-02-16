/**
 * @file Tests for \ref donner::svg::MaskUnits and \ref donner::svg::MaskContentUnits enums and its
 * ostream output operators.
 */

#include "donner/svg/core/MaskUnits.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref MaskUnits values.
TEST(MaskUnitsTest, OstreamOutput) {
  EXPECT_THAT(MaskUnits::UserSpaceOnUse, ToStringIs("MaskUnits::UserSpaceOnUse"));
  EXPECT_THAT(MaskUnits::ObjectBoundingBox, ToStringIs("MaskUnits::ObjectBoundingBox"));
  EXPECT_THAT(MaskUnits::Default, ToStringIs("MaskUnits::ObjectBoundingBox"));
}

TEST(MaskUnitsTest, MaskContentUnitsOstreamOutput) {
  EXPECT_THAT(MaskContentUnits::UserSpaceOnUse, ToStringIs("MaskContentUnits::UserSpaceOnUse"));
  EXPECT_THAT(MaskContentUnits::ObjectBoundingBox,
              ToStringIs("MaskContentUnits::ObjectBoundingBox"));
  EXPECT_THAT(MaskContentUnits::Default, ToStringIs("MaskContentUnits::UserSpaceOnUse"));
}

}  // namespace donner::svg
