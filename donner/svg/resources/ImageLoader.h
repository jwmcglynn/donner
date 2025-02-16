#pragma once
/// @file

#include "donner/svg/resources/ImageResource.h"
#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg {

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
  explicit ImageLoader(ResourceLoaderInterface& resourceLoader) : urlLoader_(resourceLoader) {}

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
   * @return A variant containing either the loaded ImageResource or an UrlLoaderError.
   */
  std::variant<ImageResource, UrlLoaderError> fromUri(std::string_view uri);

private:
  /// Loader used for decoding the data URL or fetching the external resources.
  UrlLoader urlLoader_;
};

}  // namespace donner::svg
