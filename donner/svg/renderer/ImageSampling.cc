#include "donner/svg/renderer/ImageSampling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"
#include "donner/svg/renderer/PixelFormatUtils.h"

namespace donner::svg {
namespace {

std::int64_t PixelatedIntegerScale(double scale) {
  constexpr double kMaxScale = 65536.0;
  return static_cast<std::int64_t>(std::clamp(std::floor(scale + 0.5), 1.0, kMaxScale));
}

bool IsFiniteTransform(const Transform2d& transform) {
  for (double component : transform.data) {
    if (!std::isfinite(component)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::vector<std::uint8_t> RasterizeImagePremultiplied(
    std::span<const std::uint8_t> premultipliedPixels, int sourceWidth, int sourceHeight,
    const Transform2d& deviceFromImage, int outputWidth, int outputHeight,
    ImageRendering imageRendering) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0 ||
      sourceWidth > kMaxImageSamplingDimension || sourceHeight > kMaxImageSamplingDimension ||
      outputWidth > kMaxImageSamplingDimension || outputHeight > kMaxImageSamplingDimension) {
    return {};
  }

  if (!HasExactRgbaPayload(premultipliedPixels, sourceWidth, sourceHeight)) {
    return {};
  }

  const std::size_t sizeOutputWidth = static_cast<std::size_t>(outputWidth);
  const std::size_t sizeOutputHeight = static_cast<std::size_t>(outputHeight);
  if (sizeOutputWidth > kMaxImageSamplingSurfacePixels / sizeOutputHeight) {
    return {};
  }
  const std::size_t outputPixels = sizeOutputWidth * sizeOutputHeight;

  std::vector<std::uint8_t> output(outputPixels * 4u, 0);
  if (!IsFiniteTransform(deviceFromImage)) {
    return output;
  }

  const double determinant = deviceFromImage.determinant();
  if (!std::isfinite(determinant) || NearZero(determinant, 1e-12)) {
    return output;
  }

  const Transform2d imageFromDevice = deviceFromImage.inverse();
  if (!IsFiniteTransform(imageFromDevice)) {
    return output;
  }

  std::int64_t pixelatedMultipleX = 1;
  std::int64_t pixelatedMultipleY = 1;
  if (imageRendering == ImageRendering::Pixelated) {
    const double scaleX = deviceFromImage.transformVector(Vector2d(1.0, 0.0)).length();
    const double scaleY = deviceFromImage.transformVector(Vector2d(0.0, 1.0)).length();
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY)) {
      return output;
    }
    pixelatedMultipleX = PixelatedIntegerScale(scaleX);
    pixelatedMultipleY = PixelatedIntegerScale(scaleY);
  }

  const auto sampleSource = [&](int x, int y, int channel) -> float {
    x = std::clamp(x, 0, sourceWidth - 1);
    y = std::clamp(y, 0, sourceHeight - 1);
    const std::size_t sourceIndex =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(sourceWidth) +
         static_cast<std::size_t>(x)) *
            4u +
        static_cast<std::size_t>(channel);
    return premultipliedPixels[sourceIndex] / 255.0f;
  };

  for (int y = 0; y < outputHeight; ++y) {
    for (int x = 0; x < outputWidth; ++x) {
      const Vector2d devicePoint(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      const Vector2d imagePoint = imageFromDevice.transformPosition(devicePoint);
      const double sourceX = imagePoint.x - 0.5;
      const double sourceY = imagePoint.y - 0.5;
      if (!std::isfinite(sourceX) || !std::isfinite(sourceY) || sourceX < -0.5 ||
          sourceX >= sourceWidth - 0.5 || sourceY < -0.5 || sourceY >= sourceHeight - 0.5) {
        continue;
      }
      const std::size_t outIndex =
          (static_cast<std::size_t>(y) * sizeOutputWidth + static_cast<std::size_t>(x)) * 4u;

      if (imageRendering == ImageRendering::CrispEdges ||
          imageRendering == ImageRendering::OptimizeSpeed) {
        const int nx = std::clamp(static_cast<int>(std::floor(sourceX + 0.5)), 0, sourceWidth - 1);
        const int ny = std::clamp(static_cast<int>(std::floor(sourceY + 0.5)), 0, sourceHeight - 1);
        for (int channel = 0; channel < 4; ++channel) {
          const std::size_t sourceIndex =
              (static_cast<std::size_t>(ny) * static_cast<std::size_t>(sourceWidth) +
               static_cast<std::size_t>(nx)) *
                  4u +
              static_cast<std::size_t>(channel);
          output[outIndex + static_cast<std::size_t>(channel)] = premultipliedPixels[sourceIndex];
        }
        continue;
      }

      if (imageRendering == ImageRendering::Pixelated) {
        const double intermediateX =
            (sourceX + 0.5) * static_cast<double>(pixelatedMultipleX) - 0.5;
        const double intermediateY =
            (sourceY + 0.5) * static_cast<double>(pixelatedMultipleY) - 0.5;
        if (!std::isfinite(intermediateX) || !std::isfinite(intermediateY)) {
          continue;
        }
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
          output[outIndex + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
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
        output[outIndex + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
            std::round(std::clamp(std::lerp(top, bottom, fy), 0.0f, 1.0f) * 255.0f));
      }
    }
  }

  return output;
}

}  // namespace donner::svg
