#include "src/svg/renderer/tests/renderer_test_utils.h"

#include <stb/stb_image.h>

namespace donner::svg {

std::optional<Image> RendererTestUtils::readRgbaImageFromPngFile(const char* filename) {
  int width, height, channels;
  auto data = stbi_load(filename, &width, &height, &channels, 4);
  if (!data) {
    ADD_FAILURE() << "Failed to load image: " << filename;
    return std::nullopt;
  }

  Image result{width, height, static_cast<size_t>(width),
               std::vector<uint8_t>(data, data + static_cast<ptrdiff_t>(width * height * 4))};
  stbi_image_free(data);
  return result;
}

}  // namespace donner::svg
