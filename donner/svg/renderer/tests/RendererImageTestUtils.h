/// @file
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace donner::svg {

/**
 * @brief Stores an uncompressed RGBA-format image.
 */
struct Image {
  int width;                  //!< Image width in pixels.
  int height;                 //!< Image height in pixels.
  size_t strideInPixels;      //!< Row stride in pixels.
  std::vector<uint8_t> data;  //!< Pixel data in RGBA format.
};

/**
 * @brief Test utilities for loading golden images.
 */
class RendererImageTestUtils {
public:
  /**
   * @brief Reads an RGBA image from a PNG file.
   *
   * @param filename Path to a PNG file to load.
   * @return Loaded image, or `std::nullopt` on failure.
   */
  static std::optional<Image> readRgbaImageFromPngFile(const char* filename);
};

}  // namespace donner::svg
