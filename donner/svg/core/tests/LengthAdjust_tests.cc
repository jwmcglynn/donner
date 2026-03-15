/**
 * @file Tests for \ref donner::svg::LengthAdjust enum and its ostream output operator.
 */

#include "donner/svg/core/LengthAdjust.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref LengthAdjust values.
TEST(LengthAdjustTest, OstreamOutput) {
  EXPECT_THAT(LengthAdjust::Spacing, ToStringIs("LengthAdjust::Spacing"));
  EXPECT_THAT(LengthAdjust::SpacingAndGlyphs, ToStringIs("LengthAdjust::SpacingAndGlyphs"));
}

/// @test Default value aliases to Spacing.
TEST(LengthAdjustTest, DefaultValue) {
  EXPECT_EQ(LengthAdjust::Default, LengthAdjust::Spacing);
}

}  // namespace donner::svg
