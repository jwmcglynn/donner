#include "donner/backends/tiny_skia_cpp/Mask.h"

#include <span>

#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {

TEST(MaskTests, CreatesValidMask) {
  Mask mask = Mask::Create(2, 3);
  ASSERT_TRUE(mask.isValid());
  EXPECT_EQ(mask.width(), 2);
  EXPECT_EQ(mask.height(), 3);
  EXPECT_EQ(mask.strideBytes(), 2u);
  EXPECT_EQ(mask.pixels().size(), 6u);
}

TEST(MaskTests, RejectsInvalidDimensions) {
  Mask mask = Mask::Create(-1, 5);
  EXPECT_FALSE(mask.isValid());
}

TEST(MaskTests, ClearsCoverage) {
  Mask mask = Mask::Create(2, 2);
  ASSERT_TRUE(mask.isValid());

  mask.clear(128);
  const std::span<const uint8_t> pixels = mask.pixels();
  EXPECT_EQ(pixels[0], 128);
  EXPECT_EQ(pixels[1], 128);
  EXPECT_EQ(pixels[2], 128);
  EXPECT_EQ(pixels[3], 128);
}

}  // namespace donner::backends::tiny_skia_cpp
