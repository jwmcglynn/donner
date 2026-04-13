/**
 * Tests for \ref donner::svg::TextDecoration enum and its ostream output operator.
 */

#include "donner/svg/core/TextDecoration.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref TextDecoration values.
TEST(TextDecorationTest, OstreamOutput) {
  EXPECT_THAT(TextDecoration::None, ToStringIs("none"));
  EXPECT_THAT(TextDecoration::Underline, ToStringIs("underline"));
  EXPECT_THAT(TextDecoration::Overline, ToStringIs("overline"));
  EXPECT_THAT(TextDecoration::LineThrough, ToStringIs("line-through"));
}

}  // namespace donner::svg
