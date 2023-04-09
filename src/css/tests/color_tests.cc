#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/color.h"

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

}  // namespace donner::css
