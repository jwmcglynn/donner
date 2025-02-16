#include "donner/svg/resources/ImageLoader.h"

#include <stb/stb_image.h>

namespace donner::svg {

namespace {

std::variant<ImageResource, UrlLoaderError> LoadImage(std::string_view mimeType,
                                                      const std::vector<uint8_t>& fileContents) {
  // Allow known formats and an empty mime type (stb_image will auto-detect)
  if (mimeType != "" && mimeType != "image/png" && mimeType != "image/jpeg" &&
      mimeType != "image/jpg" && mimeType != "image/gif") {
    return UrlLoaderError::UnsupportedFormat;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t* data =
      stbi_load_from_memory(reinterpret_cast<const unsigned char*>(
                                fileContents.data()),  // NOLINT, allow reinterpret_cast.
                            fileContents.size(), &width, &height, &channels, 4);
  if (!data) {
    return UrlLoaderError::DataCorrupt;
  }

  const size_t dataSize = static_cast<size_t>(width) * height * 4;

  ImageResource result;
  result.data = std::vector<uint8_t>(data, data + dataSize);
  result.width = width;
  result.height = height;

  stbi_image_free(data);

  return result;
}

}  // namespace

std::variant<ImageResource, UrlLoaderError> ImageLoader::fromUri(std::string_view uri) {
  auto urlResultOrError = urlLoader_.fromUri(uri);
  if (std::holds_alternative<UrlLoaderError>(urlResultOrError)) {
    return std::get<UrlLoaderError>(urlResultOrError);
  }

  const UrlLoader::Result& urlResult = std::get<UrlLoader::Result>(urlResultOrError);

  return LoadImage(urlResult.mimeType, urlResult.data);
}

}  // namespace donner::svg
