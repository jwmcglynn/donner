/**
 * Tests for \ref donner::svg::TransformOrigin and its ostream output operator.
 */

#include "donner/svg/core/TransformOrigin.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for \ref TransformOrigin.
TEST(TransformOriginTest, OstreamOutput) {
  {
    TransformOrigin origin{Lengthd(10, Lengthd::Unit::Px), Lengthd(20, Lengthd::Unit::Px)};
    EXPECT_THAT(origin, ToStringIs("10px 20px"));
  }

  {
    TransformOrigin origin{Lengthd(50, Lengthd::Unit::Percent), Lengthd(50, Lengthd::Unit::Percent)};
    EXPECT_THAT(origin, ToStringIs("50% 50%"));
  }

  {
    TransformOrigin origin{Lengthd(0, Lengthd::Unit::None), Lengthd(0, Lengthd::Unit::None)};
    EXPECT_THAT(origin, ToStringIs("0 0"));
  }
}

}  // namespace donner::svg
