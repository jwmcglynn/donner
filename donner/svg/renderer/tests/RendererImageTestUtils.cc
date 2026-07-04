#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

#include <gtest/gtest.h>
#include <stb/stb_image.h>

#include <optional>

namespace donner::svg {

namespace {

constexpr int kMaxImageDimension = 16384;
constexpr size_t kMaxDecodedImageBytes = 256u * 1024u * 1024u;

std::optional<size_t> RgbaByteSize(int width, int height) {
  if (width <= 0 || height <= 0 || width > kMaxImageDimension || height > kMaxImageDimension) {
    return std::nullopt;
  }

  constexpr size_t kRgbaChannels = 4u;
  const size_t widthSize = static_cast<size_t>(width);
  const size_t heightSize = static_cast<size_t>(height);
  if (widthSize > kMaxDecodedImageBytes / heightSize / kRgbaChannels) {
    return std::nullopt;
  }

  return widthSize * heightSize * kRgbaChannels;
}

}  // namespace

std::optional<Image> RendererImageTestUtils::readRgbaImageFromPngFile(const char* filename) {
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info(filename, &width, &height, &channels) == 0) {
    ADD_FAILURE() << "Failed to read image info: " << filename;
    return std::nullopt;
  }

  std::optional<size_t> dataSize = RgbaByteSize(width, height);
  if (!dataSize.has_value()) {
    ADD_FAILURE() << "Unsupported image dimensions for " << filename << ": " << width << "x"
                  << height;
    return std::nullopt;
  }

  auto* data = stbi_load(filename, &width, &height, &channels, 4);
  if (data == nullptr) {
    ADD_FAILURE() << "Failed to load image: " << filename;
    return std::nullopt;
  }
  dataSize = RgbaByteSize(width, height);
  if (!dataSize.has_value()) {
    stbi_image_free(data);
    ADD_FAILURE() << "Unsupported decoded image dimensions for " << filename << ": " << width << "x"
                  << height;
    return std::nullopt;
  }

  Image result{
      width,
      height,
      static_cast<size_t>(width),
      std::vector<uint8_t>(data, data + *dataSize),
  };
  stbi_image_free(data);
  return result;
}

}  // namespace donner::svg
