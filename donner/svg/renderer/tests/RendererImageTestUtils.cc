#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

#include <gtest/gtest.h>
#include <stb/stb_image.h>

namespace donner::svg {

std::optional<Image> RendererImageTestUtils::readRgbaImageFromPngFile(const char* filename) {
  int width = 0;
  int height = 0;
  int channels = 0;
  auto* data = stbi_load(filename, &width, &height, &channels, 4);
  if (data == nullptr) {
    ADD_FAILURE() << "Failed to load image: " << filename;
    return std::nullopt;
  }

  Image result{
      width,
      height,
      static_cast<size_t>(width),
      std::vector<uint8_t>(data, data + static_cast<std::ptrdiff_t>(width * height * 4)),
  };
  stbi_image_free(data);
  return result;
}

}  // namespace donner::svg
