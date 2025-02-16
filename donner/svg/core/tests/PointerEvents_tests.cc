/**
 * @file Tests for \ref donner::svg::PointerEvents enum and its ostream output operator.
 */

#include "donner/svg/core/PointerEvents.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref PointerEvents values.
TEST(PointerEventsTest, OstreamOutput) {
  EXPECT_THAT(PointerEvents::None, ToStringIs("none"));
  EXPECT_THAT(PointerEvents::BoundingBox, ToStringIs("bounding-box"));
  EXPECT_THAT(PointerEvents::VisiblePainted, ToStringIs("visiblePainted"));
  EXPECT_THAT(PointerEvents::VisibleFill, ToStringIs("visibleFill"));
  EXPECT_THAT(PointerEvents::VisibleStroke, ToStringIs("visibleStroke"));
  EXPECT_THAT(PointerEvents::Visible, ToStringIs("visible"));
  EXPECT_THAT(PointerEvents::Painted, ToStringIs("painted"));
  EXPECT_THAT(PointerEvents::Fill, ToStringIs("fill"));
  EXPECT_THAT(PointerEvents::Stroke, ToStringIs("stroke"));
  EXPECT_THAT(PointerEvents::All, ToStringIs("all"));
}

}  // namespace donner::svg
