#include "donner/backends/tiny_skia_cpp/Pixmap.h"

#include <limits>
#include <utility>

namespace donner::backends::tiny_skia_cpp {

Pixmap::Pixmap(int width, int height, size_t strideBytes, std::vector<uint8_t>&& pixels)
    : width_(width), height_(height), strideBytes_(strideBytes), pixels_(std::move(pixels)) {}

Pixmap Pixmap::Create(int width, int height) {
  if (width <= 0 || height <= 0) {
    return Pixmap();
  }

  const size_t widthSize = static_cast<size_t>(width);
  const size_t heightSize = static_cast<size_t>(height);

  if (widthSize > std::numeric_limits<size_t>::max() / kBytesPerPixel) {
    return Pixmap();
  }

  const size_t rowBytes = widthSize * kBytesPerPixel;
  if (heightSize > std::numeric_limits<size_t>::max() / rowBytes) {
    return Pixmap();
  }

  const size_t totalBytes = rowBytes * heightSize;
  return Pixmap(width, height, rowBytes, std::vector<uint8_t>(totalBytes, 0));
}

}  // namespace donner::backends::tiny_skia_cpp
