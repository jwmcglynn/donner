#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

#include <gtest/gtest.h>

#include "tools/svg_suite/ImageCompare.h"

namespace donner::svg {

std::optional<Image> RendererImageTestUtils::readRgbaImageFromPngFile(const char* filename) {
  svg_suite::ImageResult loaded = svg_suite::ReadPng(filename);
  if (!loaded.image) {
    ADD_FAILURE() << "Failed to read PNG " << filename << ": " << loaded.error;
    return std::nullopt;
  }
  auto& image = *loaded.image;
  return Image{image.width, image.height, image.strideInPixels, std::move(image.data)};
}

}  // namespace donner::svg
