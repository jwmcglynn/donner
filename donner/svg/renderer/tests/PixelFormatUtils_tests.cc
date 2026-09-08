#include "donner/svg/renderer/PixelFormatUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace donner::svg {
namespace {

TEST(PixelFormatUtils, UnpremultiplyMatchesDoubleReferenceForEveryBytePair) {
  std::vector<uint8_t> rgba;
  std::vector<uint8_t> expected;
  rgba.reserve(256 * 4);
  expected.reserve(256 * 4);
  for (uint32_t alpha = 0; alpha <= 255; ++alpha) {
    SCOPED_TRACE(alpha);
    rgba.clear();
    expected.clear();
    const uint8_t a = static_cast<uint8_t>(alpha);
    for (uint32_t channel = 0; channel <= 255; ++channel) {
      const uint8_t c = static_cast<uint8_t>(channel);
      const long rounded = alpha == 0 ? 0 : std::lround(255.0 * channel / alpha);
      const uint8_t straight = static_cast<uint8_t>(std::min(255L, rounded));
      rgba.insert(rgba.end(), {c, c, c, a});
      expected.insert(expected.end(), {straight, straight, straight, a});
    }
    UnpremultiplyRgbaInPlace(rgba);
    EXPECT_THAT(rgba, testing::ElementsAreArray(expected));
  }
}

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
