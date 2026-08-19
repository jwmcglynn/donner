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

struct SourceImage {
  std::span<const std::uint8_t> pixels;
  int width = 0;
  int height = 0;
};

struct PixelatedSampling {
  std::int64_t multipleX = 1;
  std::int64_t multipleY = 1;
  std::int64_t intermediateWidth = 0;
  std::int64_t intermediateHeight = 0;
};

bool IsValidSamplingRequest(std::span<const std::uint8_t> pixels, int sourceWidth, int sourceHeight,
                            int outputWidth, int outputHeight, std::size_t& outputPixels) {
  if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0 ||
      outputWidth > kMaxImageSamplingDimension || outputHeight > kMaxImageSamplingDimension ||
      !HasExactRgbaPayload(pixels, sourceWidth, sourceHeight)) {
    return false;
  }

  const std::size_t width = static_cast<std::size_t>(outputWidth);
  const std::size_t height = static_cast<std::size_t>(outputHeight);
  if (width > kMaxImageSamplingSurfacePixels / height) {
    return false;
  }

  outputPixels = width * height;
  return true;
}

bool ResolveImageFromDevice(const Transform2d& deviceFromImage, Transform2d& imageFromDevice) {
  if (!IsFiniteTransform(deviceFromImage)) {
    return false;
  }

  const double determinant = deviceFromImage.determinant();
  if (!std::isfinite(determinant) || NearZero(determinant, 1e-12)) {
    return false;
  }

  imageFromDevice = deviceFromImage.inverse();
  return IsFiniteTransform(imageFromDevice);
}

bool ResolvePixelatedSampling(const Transform2d& deviceFromImage, const SourceImage& source,
                              ImageRendering imageRendering, PixelatedSampling& sampling) {
  if (imageRendering != ImageRendering::Pixelated) {
    return true;
  }

  const double scaleX = deviceFromImage.transformVector(Vector2d(1.0, 0.0)).length();
  const double scaleY = deviceFromImage.transformVector(Vector2d(0.0, 1.0)).length();
  if (!std::isfinite(scaleX) || !std::isfinite(scaleY)) {
    return false;
  }

  sampling.multipleX = PixelatedIntegerScale(scaleX);
  sampling.multipleY = PixelatedIntegerScale(scaleY);
  sampling.intermediateWidth = static_cast<std::int64_t>(source.width) * sampling.multipleX;
  sampling.intermediateHeight = static_cast<std::int64_t>(source.height) * sampling.multipleY;
  return true;
}

bool MapOutputPixelToSource(const Transform2d& imageFromDevice, int outputX, int outputY,
                            const SourceImage& source, double& sourceX, double& sourceY) {
  const Vector2d devicePoint(static_cast<double>(outputX) + 0.5,
                             static_cast<double>(outputY) + 0.5);
  const Vector2d imagePoint = imageFromDevice.transformPosition(devicePoint);
  sourceX = imagePoint.x - 0.5;
  sourceY = imagePoint.y - 0.5;
  return std::isfinite(sourceX) && std::isfinite(sourceY) && sourceX >= -0.5 &&
         sourceX < source.width - 0.5 && sourceY >= -0.5 && sourceY < source.height - 0.5;
}

float SampleSource(const SourceImage& source, int x, int y, int channel) {
  x = std::clamp(x, 0, source.width - 1);
  y = std::clamp(y, 0, source.height - 1);
  const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) +
                             static_cast<std::size_t>(x)) *
                                4u +
                            static_cast<std::size_t>(channel);
  return source.pixels[index] / 255.0f;
}

void WriteNearestPixel(const SourceImage& source, double sourceX, double sourceY,
                       std::span<std::uint8_t> output, std::size_t outputIndex) {
  const int nearestX = std::clamp(static_cast<int>(std::floor(sourceX + 0.5)), 0, source.width - 1);
  const int nearestY =
      std::clamp(static_cast<int>(std::floor(sourceY + 0.5)), 0, source.height - 1);
  const std::size_t sourceIndex =
      (static_cast<std::size_t>(nearestY) * static_cast<std::size_t>(source.width) +
       static_cast<std::size_t>(nearestX)) *
      4u;
  std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(sourceIndex), 4,
              output.begin() + static_cast<std::ptrdiff_t>(outputIndex));
}

void WriteBilinearPixel(const SourceImage& source, double sourceX, double sourceY,
                        std::span<std::uint8_t> output, std::size_t outputIndex) {
  const int x0 = static_cast<int>(std::floor(sourceX));
  const int y0 = static_cast<int>(std::floor(sourceY));
  const float fractionX = static_cast<float>(sourceX - x0);
  const float fractionY = static_cast<float>(sourceY - y0);
  for (int channel = 0; channel < 4; ++channel) {
    const float top = std::lerp(SampleSource(source, x0, y0, channel),
                                SampleSource(source, x0 + 1, y0, channel), fractionX);
    const float bottom = std::lerp(SampleSource(source, x0, y0 + 1, channel),
                                   SampleSource(source, x0 + 1, y0 + 1, channel), fractionX);
    output[outputIndex + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
        std::round(std::clamp(std::lerp(top, bottom, fractionY), 0.0f, 1.0f) * 255.0f));
  }
}

void WritePixelatedPixel(const SourceImage& source, const PixelatedSampling& sampling,
                         double sourceX, double sourceY, std::span<std::uint8_t> output,
                         std::size_t outputIndex) {
  const double intermediateX = (sourceX + 0.5) * static_cast<double>(sampling.multipleX) - 0.5;
  const double intermediateY = (sourceY + 0.5) * static_cast<double>(sampling.multipleY) - 0.5;
  if (!std::isfinite(intermediateX) || !std::isfinite(intermediateY)) {
    return;
  }

  const std::int64_t x0 = static_cast<std::int64_t>(std::floor(intermediateX));
  const std::int64_t y0 = static_cast<std::int64_t>(std::floor(intermediateY));
  const float fractionX = static_cast<float>(intermediateX - static_cast<double>(x0));
  const float fractionY = static_cast<float>(intermediateY - static_cast<double>(y0));
  const auto sampleIntermediate = [&](std::int64_t x, std::int64_t y, int channel) {
    x = std::clamp<std::int64_t>(x, 0, sampling.intermediateWidth - 1);
    y = std::clamp<std::int64_t>(y, 0, sampling.intermediateHeight - 1);
    return SampleSource(source, static_cast<int>(x / sampling.multipleX),
                        static_cast<int>(y / sampling.multipleY), channel);
  };

  for (int channel = 0; channel < 4; ++channel) {
    const float top = std::lerp(sampleIntermediate(x0, y0, channel),
                                sampleIntermediate(x0 + 1, y0, channel), fractionX);
    const float bottom = std::lerp(sampleIntermediate(x0, y0 + 1, channel),
                                   sampleIntermediate(x0 + 1, y0 + 1, channel), fractionX);
    output[outputIndex + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(
        std::round(std::clamp(std::lerp(top, bottom, fractionY), 0.0f, 1.0f) * 255.0f));
  }
}

void WriteSampledPixel(const SourceImage& source, const PixelatedSampling& pixelatedSampling,
                       ImageRendering imageRendering, double sourceX, double sourceY,
                       std::span<std::uint8_t> output, std::size_t outputIndex) {
  if (imageRendering == ImageRendering::CrispEdges ||
      imageRendering == ImageRendering::OptimizeSpeed) {
    WriteNearestPixel(source, sourceX, sourceY, output, outputIndex);
  } else if (imageRendering == ImageRendering::Pixelated) {
    WritePixelatedPixel(source, pixelatedSampling, sourceX, sourceY, output, outputIndex);
  } else {
    WriteBilinearPixel(source, sourceX, sourceY, output, outputIndex);
  }
}

void RasterizePixels(const SourceImage& source, const Transform2d& imageFromDevice,
                     const PixelatedSampling& pixelatedSampling, ImageRendering imageRendering,
                     int outputWidth, int outputHeight, std::span<std::uint8_t> output) {
  const std::size_t sizeOutputWidth = static_cast<std::size_t>(outputWidth);
  for (int y = 0; y < outputHeight; ++y) {
    for (int x = 0; x < outputWidth; ++x) {
      double sourceX = 0.0;
      double sourceY = 0.0;
      if (!MapOutputPixelToSource(imageFromDevice, x, y, source, sourceX, sourceY)) {
        continue;
      }

      const std::size_t outputIndex =
          (static_cast<std::size_t>(y) * sizeOutputWidth + static_cast<std::size_t>(x)) * 4u;
      WriteSampledPixel(source, pixelatedSampling, imageRendering, sourceX, sourceY, output,
                        outputIndex);
    }
  }
}

}  // namespace

std::vector<std::uint8_t> RasterizeImagePremultiplied(
    std::span<const std::uint8_t> premultipliedPixels, int sourceWidth, int sourceHeight,
    const Transform2d& deviceFromImage, int outputWidth, int outputHeight,
    ImageRendering imageRendering) {
  std::size_t outputPixels = 0;
  if (!IsValidSamplingRequest(premultipliedPixels, sourceWidth, sourceHeight, outputWidth,
                              outputHeight, outputPixels)) {
    return {};
  }

  std::vector<std::uint8_t> output(outputPixels * 4u, 0);
  Transform2d imageFromDevice;
  const SourceImage source{premultipliedPixels, sourceWidth, sourceHeight};
  PixelatedSampling pixelatedSampling;
  if (!ResolveImageFromDevice(deviceFromImage, imageFromDevice) ||
      !ResolvePixelatedSampling(deviceFromImage, source, imageRendering, pixelatedSampling)) {
    return output;
  }

  RasterizePixels(source, imageFromDevice, pixelatedSampling, imageRendering, outputWidth,
                  outputHeight, output);
  return output;
}

}  // namespace donner::svg
