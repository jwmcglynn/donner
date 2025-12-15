#include "donner/backends/tiny_skia_cpp/Mask.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace donner::backends::tiny_skia_cpp {

Mask::Mask(int width, int height, size_t strideBytes, std::vector<uint8_t>&& pixels)
    : width_(width), height_(height), strideBytes_(strideBytes), pixels_(std::move(pixels)) {}

Mask Mask::Create(int width, int height) {
  if (width <= 0 || height <= 0) {
    return Mask();
  }

  const size_t widthSize = static_cast<size_t>(width);
  const size_t heightSize = static_cast<size_t>(height);

  if (widthSize > std::numeric_limits<size_t>::max() / heightSize) {
    return Mask();
  }

  const size_t rowBytes = widthSize;
  const size_t totalBytes = rowBytes * heightSize;
  return Mask(width, height, rowBytes, std::vector<uint8_t>(totalBytes, 0));
}

void Mask::clear(uint8_t coverage) {
  std::fill(pixels_.begin(), pixels_.end(), coverage);
}

}  // namespace donner::backends::tiny_skia_cpp
