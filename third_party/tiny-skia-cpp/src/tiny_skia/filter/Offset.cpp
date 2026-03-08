#include "tiny_skia/filter/Offset.h"

#include <algorithm>
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

}  // namespace tiny_skia::filter
