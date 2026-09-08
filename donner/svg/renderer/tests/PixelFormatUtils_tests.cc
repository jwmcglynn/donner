#include "donner/svg/renderer/PixelFormatUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace donner::svg {
namespace {

TEST(PixelFormatUtils, SaturatedColorsStaySaturatedAtEveryNonzeroAlpha) {
  for (uint32_t alpha = 1; alpha <= 255; ++alpha) {
    SCOPED_TRACE(alpha);
    const uint8_t a = static_cast<uint8_t>(alpha);
    std::vector<uint8_t> rgba{a, a, a, a};
    UnpremultiplyRgbaInPlace(rgba);
    EXPECT_THAT(rgba, testing::ElementsAre(255, 255, 255, a));
  }
}

TEST(PixelFormatUtils, UnpremultiplyRoundsHalfUpAndClampsInvalidPremultipliedChannels) {
  std::vector<uint8_t> rgba{1, 2, 3, 6, 191, 255, 0, 191};
  UnpremultiplyRgbaInPlace(rgba);
  EXPECT_THAT(rgba, testing::ElementsAre(43, 85, 128, 6, 255, 255, 0, 191));
}

TEST(PixelFormatUtils, TransparentPixelsClearColorAndOpaquePixelsKeepTheirBytes) {
  std::vector<uint8_t> rgba{255, 100, 1, 0, 13, 79, 253, 255};
  UnpremultiplyRgbaInPlace(rgba);
  EXPECT_THAT(rgba, testing::ElementsAre(0, 0, 0, 0, 13, 79, 253, 255));
}

TEST(PixelFormatUtils, RowConversionPreservesExactSaturationAndDropsPadding) {
  const std::vector<uint8_t> input{191, 0, 0, 191, 9, 9, 9, 9, 0, 191, 0, 191, 9, 9, 9, 9};
  EXPECT_THAT(UnpremultiplyRgbaRows(input, 1, 2, 8),
              testing::ElementsAre(255, 0, 0, 191, 0, 255, 0, 191));
}

}  // namespace
}  // namespace donner::svg
