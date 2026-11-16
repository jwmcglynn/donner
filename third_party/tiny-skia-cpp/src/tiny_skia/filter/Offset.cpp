#include "tiny_skia/filter/Offset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace tiny_skia::filter {

void offset(const Pixmap& src, Pixmap& dst, int dx, int dy) {
  const int width = static_cast<int>(src.width());
  const int height = static_cast<int>(src.height());

  // Clear destination to transparent black.
  auto dstData = dst.data();
  std::fill(dstData.begin(), dstData.end(), std::uint8_t{0});

  // Compute overlapping region.
  const int srcStartX = std::max(0, -dx);
  const int srcStartY = std::max(0, -dy);
  const int srcEndX = std::min(width, width - dx);
  const int srcEndY = std::min(height, height - dy);

  if (srcStartX >= srcEndX || srcStartY >= srcEndY) {
    return;  // No overlap.
  }

  const auto srcData = src.data();
  const int copyWidth = srcEndX - srcStartX;

  for (int y = srcStartY; y < srcEndY; ++y) {
    const int dstY = y + dy;
    const std::size_t srcOffset = static_cast<std::size_t>((y * width + srcStartX) * 4);
    const std::size_t dstOffset = static_cast<std::size_t>((dstY * width + srcStartX + dx) * 4);
    std::memcpy(&dstData[dstOffset], &srcData[srcOffset],
                static_cast<std::size_t>(copyWidth) * 4);
  }
}

namespace {

void offsetWholePixels(const FloatPixmap& src, FloatPixmap& dst, int dx, int dy) {
  const int width = static_cast<int>(src.width());
  const int height = static_cast<int>(src.height());

  // Compute overlapping region.
  const int srcStartX = std::max(0, -dx);
  const int srcStartY = std::max(0, -dy);
  const int srcEndX = std::min(width, width - dx);
  const int srcEndY = std::min(height, height - dy);

  if (srcStartX >= srcEndX || srcStartY >= srcEndY) {
    return;  // No overlap.
  }

  const auto srcData = src.data();
  auto dstData = dst.data();
  const int copyWidth = srcEndX - srcStartX;

  for (int y = srcStartY; y < srcEndY; ++y) {
    const int dstY = y + dy;
    const std::size_t srcOffset = static_cast<std::size_t>((y * width + srcStartX) * 4);
    const std::size_t dstOffset = static_cast<std::size_t>((dstY * width + srcStartX + dx) * 4);
    std::memcpy(&dstData[dstOffset], &srcData[srcOffset],
                static_cast<std::size_t>(copyWidth) * 4 * sizeof(float));
  }
}

/// Adds \p weight times the pixel of \p src at (\p x, \p y) to \p sum. Pixels outside \p src
/// are transparent black.
void accumulatePixel(const FloatPixmap& src, int x, int y, float weight,
                     std::array<float, 4>& sum) {
  const int width = static_cast<int>(src.width());
  if (weight == 0.0f || x < 0 || y < 0 || x >= width || y >= static_cast<int>(src.height())) {
    return;
  }
  const auto srcData = src.data();
  const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
  for (int channel = 0; channel < 4; ++channel) {
    sum[channel] += weight * srcData[index + channel];
  }
}

}  // namespace

void offset(const FloatPixmap& src, FloatPixmap& dst, double dx, double dy) {
  // Clear destination to transparent black.
  dst.clear();

  const int width = static_cast<int>(src.width());
  const int height = static_cast<int>(src.height());
  if (!std::isfinite(dx) || !std::isfinite(dy) || std::abs(dx) >= width ||
      std::abs(dy) >= height) {
    return;  // No overlap.
  }

  // dx = wholeX + fractionX with fractionX in [0, 1): destination pixel x reads source pixels
  // x - wholeX (weight 1 - fractionX) and x - wholeX - 1 (weight fractionX).
  const double floorX = std::floor(dx);
  const double floorY = std::floor(dy);
  const int wholeX = static_cast<int>(floorX);
  const int wholeY = static_cast<int>(floorY);
  const float fractionX = static_cast<float>(dx - floorX);
  const float fractionY = static_cast<float>(dy - floorY);
  if (fractionX == 0.0f && fractionY == 0.0f) {
    offsetWholePixels(src, dst, wholeX, wholeY);
    return;
  }

  const std::array<float, 2> weightX = {1.0f - fractionX, fractionX};
  const std::array<float, 2> weightY = {1.0f - fractionY, fractionY};
  auto dstData = dst.data();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::array<float, 4> sum = {0.0f, 0.0f, 0.0f, 0.0f};
      for (int tapY = 0; tapY < 2; ++tapY) {
        for (int tapX = 0; tapX < 2; ++tapX) {
          accumulatePixel(src, x - wholeX - tapX, y - wholeY - tapY,
                          weightX[tapX] * weightY[tapY], sum);
        }
      }
      std::copy(sum.begin(), sum.end(),
                dstData.begin() + static_cast<std::ptrdiff_t>((y * width + x) * 4));
    }
  }
}

}  // namespace tiny_skia::filter
