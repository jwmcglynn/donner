#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/Color.h"

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
TEST(Color, HSLADeferredConversion) {
  Color hslaColor = Color(Color::HSLA(180, 50, 50, 1.0));
  EXPECT_FALSE(hslaColor.hasRGBA());
  EXPECT_TRUE(hslaColor.hasHSLA());
  auto hsla = hslaColor.hsla();
  EXPECT_EQ(hsla.h, 180);
  EXPECT_EQ(hsla.s, 50);
  EXPECT_EQ(hsla.l, 50);
  EXPECT_EQ(hsla.a, 1.0);
}

}  // namespace donner::css
