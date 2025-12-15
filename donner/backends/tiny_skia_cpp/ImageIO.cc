#include "donner/backends/tiny_skia_cpp/ImageIO.h"

#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <limits>

namespace donner::backends::tiny_skia_cpp {

Expected<Pixmap, PngError> ImageIO::LoadRgbaPng(std::string_view filename) {
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* decoded = stbi_load(filename.data(), &width, &height, &channels, 4);
  if (decoded == nullptr) {
    return Expected<Pixmap, PngError>::Failure(
        {"Failed to decode PNG: " + std::string(stbi_failure_reason())});
  }

  Pixmap pixmap = Pixmap::Create(width, height);
  if (!pixmap.isValid()) {
    stbi_image_free(decoded);
    return Expected<Pixmap, PngError>::Failure({"PNG dimensions exceed limits"});
  }

  auto pixels = pixmap.pixels();
  std::copy(decoded, decoded + pixels.size(), pixels.begin());
  stbi_image_free(decoded);

  return Expected<Pixmap, PngError>::Success(std::move(pixmap));
}

Expected<std::monostate, PngError> ImageIO::WriteRgbaPng(std::string_view filename,
                                                         const Pixmap& pixmap) {
  if (!pixmap.isValid()) {
    return Expected<std::monostate, PngError>::Failure({"Pixmap is not initialized"});
  }

  if (pixmap.width() <= 0 || pixmap.height() <= 0) {
    return Expected<std::monostate, PngError>::Failure({"Pixmap dimensions must be positive"});
  }

  if (pixmap.strideBytes() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return Expected<std::monostate, PngError>::Failure({"Pixmap stride exceeds stb_image limits"});
  }

  const int result = stbi_write_png(filename.data(), pixmap.width(), pixmap.height(), 4,
                                    pixmap.data(), static_cast<int>(pixmap.strideBytes()));
  if (result == 0) {
    return Expected<std::monostate, PngError>::Failure({"Failed to encode PNG"});
  }

  return Expected<std::monostate, PngError>::Success(std::monostate{});
}

}  // namespace donner::backends::tiny_skia_cpp
