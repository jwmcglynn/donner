/**
 * @file Tests for \ref donner::svg::DominantBaseline enum and its ostream output operator.
 */

#include "donner/svg/core/DominantBaseline.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref DominantBaseline values.
TEST(DominantBaselineTest, OstreamOutput) {
  EXPECT_THAT(DominantBaseline::Auto, ToStringIs("auto"));
  EXPECT_THAT(DominantBaseline::TextBottom, ToStringIs("text-bottom"));
  EXPECT_THAT(DominantBaseline::Alphabetic, ToStringIs("alphabetic"));
  EXPECT_THAT(DominantBaseline::Ideographic, ToStringIs("ideographic"));
  EXPECT_THAT(DominantBaseline::Middle, ToStringIs("middle"));
  EXPECT_THAT(DominantBaseline::Central, ToStringIs("central"));
  EXPECT_THAT(DominantBaseline::Mathematical, ToStringIs("mathematical"));
  EXPECT_THAT(DominantBaseline::Hanging, ToStringIs("hanging"));
  EXPECT_THAT(DominantBaseline::TextTop, ToStringIs("text-top"));
}

}  // namespace donner::svg
