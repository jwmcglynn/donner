/**
 * Tests for \ref donner::svg::TextAnchor enum and its ostream output operator.
 */

#include "donner/svg/core/TextAnchor.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref TextAnchor values.
TEST(TextAnchorTest, OstreamOutput) {
  EXPECT_THAT(TextAnchor::Start, ToStringIs("start"));
  EXPECT_THAT(TextAnchor::Middle, ToStringIs("middle"));
  EXPECT_THAT(TextAnchor::End, ToStringIs("end"));
}

}  // namespace donner::svg
