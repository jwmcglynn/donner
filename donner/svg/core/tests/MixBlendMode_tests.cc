/**
 * Tests for \ref donner::svg::MixBlendMode enum and its ostream output operator.
 */

#include "donner/svg/core/MixBlendMode.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref MixBlendMode values.
TEST(MixBlendModeTest, OstreamOutput) {
  EXPECT_THAT(MixBlendMode::Normal, ToStringIs("normal"));
  EXPECT_THAT(MixBlendMode::Multiply, ToStringIs("multiply"));
  EXPECT_THAT(MixBlendMode::Screen, ToStringIs("screen"));
  EXPECT_THAT(MixBlendMode::Overlay, ToStringIs("overlay"));
  EXPECT_THAT(MixBlendMode::Darken, ToStringIs("darken"));
  EXPECT_THAT(MixBlendMode::Lighten, ToStringIs("lighten"));
  EXPECT_THAT(MixBlendMode::ColorDodge, ToStringIs("color-dodge"));
  EXPECT_THAT(MixBlendMode::ColorBurn, ToStringIs("color-burn"));
  EXPECT_THAT(MixBlendMode::HardLight, ToStringIs("hard-light"));
  EXPECT_THAT(MixBlendMode::SoftLight, ToStringIs("soft-light"));
  EXPECT_THAT(MixBlendMode::Difference, ToStringIs("difference"));
  EXPECT_THAT(MixBlendMode::Exclusion, ToStringIs("exclusion"));
  EXPECT_THAT(MixBlendMode::Hue, ToStringIs("hue"));
  EXPECT_THAT(MixBlendMode::Saturation, ToStringIs("saturation"));
  EXPECT_THAT(MixBlendMode::Color, ToStringIs("color"));
  EXPECT_THAT(MixBlendMode::Luminosity, ToStringIs("luminosity"));
}

}  // namespace donner::svg
