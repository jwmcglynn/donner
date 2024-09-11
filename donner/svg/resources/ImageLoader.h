#pragma once
/// @file

#include "donner/base/Utils.h"
#include "donner/svg/resources/ImageResource.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

/**
 * Enum of possible errors that can occur when loading an image.
 */
enum class ImageLoaderError {
  NotFound,           ///< The file was not found.
  UnsupportedFormat,  ///< The image format is not supported (mime type must be either "image/png"
                      ///< or "image/jpeg").
  InvalidDataUrl,     ///< The data URL is invalid.
  ImageCorrupt,       ///< The image data is corrupt.
};

inline std::string_view ToString(ImageLoaderError err) {
  switch (err) {
    case ImageLoaderError::NotFound: return "File not found";
    case ImageLoaderError::UnsupportedFormat: return "Unsupported format";
    case ImageLoaderError::InvalidDataUrl: return "Invalid data URL";
    case ImageLoaderError::ImageCorrupt: return "Image corrupted";
  }

  UTILS_UNREACHABLE();
}

/**
 * Utility class for loading images from a URI.
 */
class ImageLoader {
public:
  /**
   * Create a new image loader that uses the given resource loader to fetch external resources.
   *
   * @param resourceLoader Resource loader to use for fetching external resources.
   */
  explicit ImageLoader(ResourceLoaderInterface& resourceLoader) : resourceLoader_(resourceLoader) {}

  /// Destructor.
  ~ImageLoader() = default;

  // No copy or move.
  ImageLoader(const ImageLoader&) = delete;
  ImageLoader(ImageLoader&&) = delete;
  ImageLoader& operator=(const ImageLoader&) = delete;
  ImageLoader& operator=(ImageLoader&&) = delete;

  /**
   * Read an image from a URI, which can be a file path, a URL, or a data URL (e.g.
   * "data:image/png;base64,...").
   *
   * @param uri URI of the image, or data URL containing a base64 embedded image.
   */
  std::variant<ImageResource, ImageLoaderError> fromUri(std::string_view uri);

private:
  /// Resource loader to use for fetching external resources.
  ResourceLoaderInterface& resourceLoader_;  // NOLINT
};

}  // namespace donner::svg
