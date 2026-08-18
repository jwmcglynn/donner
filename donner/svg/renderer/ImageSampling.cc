#include "donner/svg/renderer/ImageSampling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"

namespace donner::svg {
namespace {

std::int64_t PixelatedIntegerScale(double scale) {
  constexpr double kMaxScale = 65536.0;
  if (!std::isfinite(scale)) {
    return static_cast<std::int64_t>(kMaxScale);
  }
  return static_cast<std::int64_t>(std::clamp(std::floor(scale + 0.5), 1.0, kMaxScale));
}

}  // namespace

std::vector<std::uint8_t> RasterizeImagePremultiplied(
    std::span<const std::uint8_t> premultipliedPixels, int sourceWidth, int sourceHeight,
    const Transform2d& deviceFromImage, int outputWidth, int outputHeight,
    ImageRendering imageRendering) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
    return {};
  }

  const std::size_t sourcePixels =
      static_cast<std::size_t>(sourceWidth) * static_cast<std::size_t>(sourceHeight);
  const std::size_t outputPixels =
      static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight);
  if (sourcePixels > premultipliedPixels.size() / 4u ||
      outputPixels > kMaxImageSamplingSurfacePixels) {
    return {};
  }

  std::vector<std::uint8_t> output(outputPixels * 4u, 0);
  const double determinant = deviceFromImage.determinant();
  if (!std::isfinite(determinant) || NearZero(determinant, 1e-12)) {
    return output;
  }

  const Transform2d imageFromDevice = deviceFromImage.inverse();
  const std::int64_t pixelatedMultipleX =
      PixelatedIntegerScale(deviceFromImage.transformVector(Vector2d(1.0, 0.0)).length());
  const std::int64_t pixelatedMultipleY =
      PixelatedIntegerScale(deviceFromImage.transformVector(Vector2d(0.0, 1.0)).length());

  const auto sampleSource = [&](int x, int y, int channel) -> float {
    x = std::clamp(x, 0, sourceWidth - 1);
    y = std::clamp(y, 0, sourceHeight - 1);
    return premultipliedPixels[static_cast<std::size_t>((y * sourceWidth + x) * 4 + channel)] /
           255.0f;
  };

  for (int y = 0; y < outputHeight; ++y) {
    for (int x = 0; x < outputWidth; ++x) {
      const Vector2d devicePoint(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      const Vector2d imagePoint = imageFromDevice.transformPosition(devicePoint);
      const double sourceX = imagePoint.x - 0.5;
      const double sourceY = imagePoint.y - 0.5;
      if (sourceX < -0.5 || sourceX >= sourceWidth - 0.5 || sourceY < -0.5 ||
          sourceY >= sourceHeight - 0.5) {
        continue;
      }
      const std::size_t outIndex = static_cast<std::size_t>((y * outputWidth + x) * 4);

      if (imageRendering == ImageRendering::CrispEdges ||
          imageRendering == ImageRendering::OptimizeSpeed) {
        const int nx = std::clamp(static_cast<int>(std::floor(sourceX + 0.5)), 0, sourceWidth - 1);
        const int ny = std::clamp(static_cast<int>(std::floor(sourceY + 0.5)), 0, sourceHeight - 1);
        for (int channel = 0; channel < 4; ++channel) {
          output[outIndex + channel] =
              premultipliedPixels[static_cast<std::size_t>((ny * sourceWidth + nx) * 4 + channel)];
        }
        continue;
      }

      if (imageRendering == ImageRendering::Pixelated) {
        const double intermediateX =
            (sourceX + 0.5) * static_cast<double>(pixelatedMultipleX) - 0.5;
        const double intermediateY =
            (sourceY + 0.5) * static_cast<double>(pixelatedMultipleY) - 0.5;
        const std::int64_t ix0 = static_cast<std::int64_t>(std::floor(intermediateX));
        const std::int64_t iy0 = static_cast<std::int64_t>(std::floor(intermediateY));
        const float fx = static_cast<float>(intermediateX - static_cast<double>(ix0));
        const float fy = static_cast<float>(intermediateY - static_cast<double>(iy0));
        const std::int64_t intermediateWidth =
            static_cast<std::int64_t>(sourceWidth) * pixelatedMultipleX;
        const std::int64_t intermediateHeight =
            static_cast<std::int64_t>(sourceHeight) * pixelatedMultipleY;
        const auto sampleIntermediate = [&](std::int64_t ix, std::int64_t iy, int channel) {
          ix = std::clamp<std::int64_t>(ix, 0, intermediateWidth - 1);
          iy = std::clamp<std::int64_t>(iy, 0, intermediateHeight - 1);
          return sampleSource(static_cast<int>(ix / pixelatedMultipleX),
                              static_cast<int>(iy / pixelatedMultipleY), channel);
        };

        for (int channel = 0; channel < 4; ++channel) {
          const float top = std::lerp(sampleIntermediate(ix0, iy0, channel),
                                      sampleIntermediate(ix0 + 1, iy0, channel), fx);
          const float bottom = std::lerp(sampleIntermediate(ix0, iy0 + 1, channel),
                                         sampleIntermediate(ix0 + 1, iy0 + 1, channel), fx);
          output[outIndex + channel] = static_cast<std::uint8_t>(
              std::round(std::clamp(std::lerp(top, bottom, fy), 0.0f, 1.0f) * 255.0f));
        }
        continue;
      }

      const int x0 = static_cast<int>(std::floor(sourceX));
      const int y0 = static_cast<int>(std::floor(sourceY));
      const float fx = static_cast<float>(sourceX - x0);
      const float fy = static_cast<float>(sourceY - y0);
      for (int channel = 0; channel < 4; ++channel) {
        const float top =
            std::lerp(sampleSource(x0, y0, channel), sampleSource(x0 + 1, y0, channel), fx);
        const float bottom =
            std::lerp(sampleSource(x0, y0 + 1, channel), sampleSource(x0 + 1, y0 + 1, channel), fx);
        output[outIndex + channel] = static_cast<std::uint8_t>(
            std::round(std::clamp(std::lerp(top, bottom, fy), 0.0f, 1.0f) * 255.0f));
      }
    }
  }

  return output;
}

}  // namespace donner::svg
