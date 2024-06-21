#include "donner/css/Color.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::css {

TEST(RGBA, ToHexString) {
  EXPECT_THAT(RGBA().toHexString(), "#ffffff");
  EXPECT_THAT(RGBA(0, 0, 0, 0).toHexString(), "#00000000");
  EXPECT_THAT(RGBA(0, 0, 0, 255).toHexString(), "#000000");
  EXPECT_THAT(RGBA(255, 255, 255, 255).toHexString(), "#ffffff");
  EXPECT_THAT(RGBA(255, 255, 255, 0).toHexString(), "#ffffff00");
  EXPECT_THAT(RGBA(0, 0, 0, 128).toHexString(), "#00000080");
  EXPECT_THAT(RGBA(255, 0, 0, 255).toHexString(), "#ff0000");
}

// Test deferred RGB conversion for HSLA
TEST(Color, HSLAtoRGBA) {
  EXPECT_EQ(HSLA::HSL(0, 0.5, 0.1).toRGBA(), RGBA(38, 13, 13, 255));
  EXPECT_EQ(HSLA::HSL(180, 0.5, 0.5).toRGBA(), RGBA(64, 191, 191, 255));
  EXPECT_EQ(HSLA::HSL(270, 0.5, 0.9).toRGBA(), RGBA(230, 217, 242, 255));
  EXPECT_EQ(HSLA::HSL(360, 0.9, 0.3).toRGBA(), RGBA(145, 8, 8, 255));
}

}  // namespace donner::css
