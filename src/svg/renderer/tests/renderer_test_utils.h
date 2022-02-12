#pragma once

#include <gtest/gtest.h>
#include <stb/stb_image.h>

#include <optional>
#include <vector>

namespace donner::svg {

struct Image {
  int width;
  int height;
  size_t strideInPixels;
  std::vector<uint8_t> data;
};

class RendererTestUtils {
public:
  static std::optional<Image> readRgbaImageFromPngFile(const char* filename) {
    int width, height, channels;
    auto data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
      ADD_FAILURE() << "Failed to load image: " << filename;
      return std::nullopt;
    }

    Image result{width, height, static_cast<size_t>(width),
                 std::vector<uint8_t>(data, data + width * height * 4)};
    stbi_image_free(data);
    return result;
  }
};

}  // namespace donner::svg
