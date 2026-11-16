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

/// The pixel of \p src at (\p x, \p y), or transparent black outside it.
std::array<float, 4> pixelOrClear(const FloatPixmap& src, int x, int y) {
  const int width = static_cast<int>(src.width());
  if (x < 0 || y < 0 || x >= width || y >= static_cast<int>(src.height())) {
    return {0.0f, 0.0f, 0.0f, 0.0f};
  }
  const auto srcData = src.data();
  const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
  return {srcData[index], srcData[index + 1], srcData[index + 2], srcData[index + 3]};
}

float lerp(float from, float to, float t) {
  return from + t * (to - from);
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

  if (dx == std::floor(dx) && dy == std::floor(dy)) {
    offsetWholePixels(src, dst, static_cast<int>(dx), static_cast<int>(dy));
    return;
  }

  // Destination pixel (x, y) samples the source at (x - dx, y - dy) and blends the four pixels
  // around that position. The arithmetic is single precision in the same order as the GPU offset
  // shaders, so both backends agree and uniform regions copy exactly. Only destination pixels
  // whose taps can reach the source are visited; the rest stay transparent.
  const float shiftX = static_cast<float>(dx);
  const float shiftY = static_cast<float>(dy);
  const int wholeX = static_cast<int>(std::floor(dx));
  const int wholeY = static_cast<int>(std::floor(dy));
  const int startX = std::max(0, wholeX);
  const int endX = std::min(width, width + wholeX + 1);
  const int startY = std::max(0, wholeY);
  const int endY = std::min(height, height + wholeY + 1);
  auto dstData = dst.data();
  for (int y = startY; y < endY; ++y) {
    const float positionY = static_cast<float>(y) - shiftY;
    const int baseY = static_cast<int>(std::floor(positionY));
    const float fractionY = positionY - std::floor(positionY);
    for (int x = startX; x < endX; ++x) {
      const float positionX = static_cast<float>(x) - shiftX;
      const int baseX = static_cast<int>(std::floor(positionX));
      const float fractionX = positionX - std::floor(positionX);
      const std::array<float, 4> topLeft = pixelOrClear(src, baseX, baseY);
      const std::array<float, 4> topRight = pixelOrClear(src, baseX + 1, baseY);
      const std::array<float, 4> bottomLeft = pixelOrClear(src, baseX, baseY + 1);
      const std::array<float, 4> bottomRight = pixelOrClear(src, baseX + 1, baseY + 1);
      const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        const float top = lerp(topLeft[channel], topRight[channel], fractionX);
        const float bottom = lerp(bottomLeft[channel], bottomRight[channel], fractionX);
        dstData[index + channel] = lerp(top, bottom, fractionY);
      }
    }
  }
}

}  // namespace tiny_skia::filter
