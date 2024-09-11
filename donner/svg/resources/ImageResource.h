#pragma once
/// @file

#include <vector>

namespace donner::svg {

struct ImageResource {
  /// Pixel data in RGBA format.
  std::vector<uint8_t> data;

  /// Width of the image, in pixels.
  int width;

  /// Height of the image, in pixels.
  int height;
};

}  // namespace donner::svg
