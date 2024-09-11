#include "donner/svg/resources/ImageLoader.h"

#include <stb/stb_image.h>

#include "donner/base/StringUtils.h"
#include "donner/svg/resources/Base64.h"

namespace donner::svg {

namespace {

std::variant<ImageResource, ImageLoaderError> LoadImage(std::string_view mimeType,
                                                        const std::vector<uint8_t>& fileContents) {
  // Allow known formats and an empty mime type (stb_image will auto-detect)
  if (mimeType != "" && mimeType != "image/png" && mimeType != "image/jpeg" &&
      mimeType != "image/jpg" && mimeType != "image/gif") {
    return ImageLoaderError::UnsupportedFormat;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  uint8_t* data = stbi_load_from_memory(fileContents.data(), fileContents.size(), &width, &height,
                                        &channels, 4);
  if (!data) {
    return ImageLoaderError::ImageCorrupt;
  }

  const size_t dataSize = static_cast<size_t>(width) * height * 4;

  ImageResource result;
  result.data = std::vector<uint8_t>(data, data + dataSize);
  result.width = width;
  result.height = height;

  stbi_image_free(data);

  return result;
}

ImageLoaderError MapError([[maybe_unused]] ResourceLoaderError error) {
  // Map all errors to NotFound for now.
  return ImageLoaderError::NotFound;
}

}  // namespace

std::variant<ImageResource, ImageLoaderError> ImageLoader::fromUri(std::string_view uri) {
  std::vector<uint8_t> imageData;

  std::string_view mimeType;

  // If the URI is of format "data:image/png;base64,...", it is a data URL.
  constexpr std::string_view dataPrefix = "data:";
  if (StringUtils::StartsWith(uri, dataPrefix)) {
    std::string_view remaining = uri.substr(dataPrefix.size());

    // Extract the mime type, until the first semicolon.
    if (const size_t mimeTypeEnd = remaining.find(';'); mimeTypeEnd != std::string::npos) {
      mimeType = remaining.substr(0, mimeTypeEnd);
      remaining.remove_prefix(mimeType.size() + 1);
    }

    // After the semicolon, look for a "base64," prefix
    constexpr std::string_view base64Prefix = "base64,";
    if (StringUtils::StartsWith(remaining, base64Prefix)) {
      remaining.remove_prefix(base64Prefix.size());
    } else {
      return ImageLoaderError::InvalidDataUrl;
    }

    auto maybeImageData = DecodeBase64Data(remaining);
    if (maybeImageData.hasResult()) {
      imageData = std::move(maybeImageData.result());
    } else {
      return ImageLoaderError::InvalidDataUrl;
    }
  } else {
    // Assume it's a file path or URL.
    auto maybeImageData = resourceLoader_.fetchExternalResource(uri);
    if (std::holds_alternative<ResourceLoaderError>(maybeImageData)) {
      return MapError(std::get<ResourceLoaderError>(maybeImageData));
    }

    imageData = std::get<std::vector<uint8_t>>(maybeImageData);
    mimeType = "image/png";  // Assume PNG for now, stb_image will auto-detect.
  }

  return LoadImage(mimeType, imageData);
}

}  // namespace donner::svg
