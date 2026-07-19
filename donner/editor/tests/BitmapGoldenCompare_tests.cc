#include "donner/editor/tests/BitmapGoldenCompare.h"

#include <array>
#include <cstdint>

#include "donner/svg/renderer/RendererInterface.h"
#include "gtest/gtest-spi.h"
#include "gtest/gtest.h"

namespace donner::editor::tests {
namespace {

svg::RendererBitmap MakeBitmap(std::array<uint8_t, 4> pixel) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(1, 1);
  bitmap.pixels.assign(pixel.begin(), pixel.end());
  bitmap.rowBytes = 4u;
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  return bitmap;
}

TEST(BitmapGoldenCompareTest, RgbaLInfToleranceAcceptsOneStepInEveryChannel) {
  constexpr std::array<uint8_t, 4> kExpectedPixel = {40u, 80u, 120u, 200u};
  const svg::RendererBitmap expected = MakeBitmap(kExpectedPixel);

  for (std::size_t channel = 0; channel < kExpectedPixel.size(); ++channel) {
    SCOPED_TRACE(channel);
    std::array<uint8_t, 4> actualPixel = kExpectedPixel;
    ++actualPixel[channel];
    CompareBitmapToBitmap(MakeBitmap(actualPixel), expected, "one-channel-step",
                          RgbaLInfToleranceParams(1, 1));
  }
}

TEST(BitmapGoldenCompareTest, RgbaLInfToleranceRejectsTwoSteps) {
  const svg::RendererBitmap expected = MakeBitmap({40u, 80u, 120u, 200u});
  const svg::RendererBitmap actual = MakeBitmap({40u, 80u, 122u, 200u});

  EXPECT_NONFATAL_FAILURE(
      CompareBitmapToBitmap(actual, expected, "two-channel-steps", RgbaLInfToleranceParams(1, 1)),
      "1 pixels exceed RGBA channel delta 1");
}

TEST(BitmapGoldenCompareTest, RgbaLInfToleranceBoundsNonIdenticalPixelCount) {
  const svg::RendererBitmap expected = MakeBitmap({40u, 80u, 120u, 200u});
  const svg::RendererBitmap actual = MakeBitmap({40u, 80u, 121u, 200u});

  EXPECT_NONFATAL_FAILURE(
      CompareBitmapToBitmap(actual, expected, "one-channel-step", RgbaLInfToleranceParams(1)),
      "1 pixels differ");
}

}  // namespace
}  // namespace donner::editor::tests
