#include "donner/svg/renderer/ImageSampling.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace donner::svg {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr std::uint8_t kOpaqueRed[] = {255, 0, 0, 255};

TEST(ImageSamplingTest, RejectsTrailingSourceBytes) {
  const std::vector<std::uint8_t> pixels = {255, 0, 0, 255, 17};

  EXPECT_THAT(
      RasterizeImagePremultiplied(pixels, 1, 1, Transform2d(), 1, 1, ImageRendering::Smooth),
      IsEmpty());
}

TEST(ImageSamplingTest, RejectsOversizedOutputAxis) {
  EXPECT_THAT(
      RasterizeImagePremultiplied(kOpaqueRed, 1, 1, Transform2d(), kMaxImageSamplingDimension + 1,
                                  1, ImageRendering::Smooth),
      IsEmpty());
}

TEST(ImageSamplingTest, NonFiniteInverseProducesSizedTransparentOutput) {
  Transform2d deviceFromImage(Transform2d::uninitialized);
  deviceFromImage.data[0] = 2e-6;
  deviceFromImage.data[1] = 1e-6;
  deviceFromImage.data[2] = 1e-6;
  deviceFromImage.data[3] = 2e-6;
  deviceFromImage.data[4] = std::numeric_limits<double>::max();
  deviceFromImage.data[5] = std::numeric_limits<double>::max();

  EXPECT_THAT(RasterizeImagePremultiplied(kOpaqueRed, 1, 1, deviceFromImage, 1, 1,
                                          ImageRendering::CrispEdges),
              ElementsAre(0, 0, 0, 0));
}

TEST(ImageSamplingTest, NonFiniteMappedCoordinatesStayTransparent) {
  const double largeFinite = std::numeric_limits<double>::max() * 0.75;
  Transform2d deviceFromImage(Transform2d::uninitialized);
  deviceFromImage.data[0] = 1.0 / largeFinite;
  deviceFromImage.data[1] = 0.0;
  deviceFromImage.data[2] = largeFinite;
  deviceFromImage.data[3] = largeFinite;
  deviceFromImage.data[4] = 0.0;
  deviceFromImage.data[5] = 0.0;

  const std::vector<std::uint8_t> output = RasterizeImagePremultiplied(
      kOpaqueRed, 1, 1, deviceFromImage, 2, 2, ImageRendering::Pixelated);
  ASSERT_EQ(output.size(), 16u);
  EXPECT_THAT(std::vector<std::uint8_t>(output.end() - 4, output.end()), ElementsAre(0, 0, 0, 0));
}

}  // namespace
}  // namespace donner::svg
