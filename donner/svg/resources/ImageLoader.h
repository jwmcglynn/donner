#pragma once
/// @file

#include "donner/svg/resources/ImageResource.h"
#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg {

/**
 * Contains raw SVG document bytes, returned by \ref ImageLoader when the referenced resource is an
 * SVG image (`image/svg+xml`). This must be parsed into an \ref SVGDocument separately.
 */
struct SvgImageContent {
  /// Raw SVG document bytes (XML text).
  std::vector<uint8_t> data;
};

/**
 * Utility class for loading images from a URI. Supports both raster images (PNG, JPEG, GIF) and
 * SVG images. Raster images are decoded into pixel data (\ref ImageResource), while SVG images
 * are returned as raw bytes (\ref SvgImageContent) for parsing by the caller.
 */
class ImageLoader {
public:
  /// Result type returned by \ref fromUri. Contains either decoded raster pixels, raw SVG content,
  /// or an error.
  using Result = std::variant<ImageResource, SvgImageContent, UrlLoaderError>;

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
   * If the resource is an SVG image (`image/svg+xml`), returns \ref SvgImageContent with raw bytes.
   * Otherwise, decodes the image into RGBA pixel data and returns \ref ImageResource.
   *
   * @param uri URI of the image, or data URL containing a base64 embedded image.
   * @return A variant containing the loaded ImageResource, SvgImageContent, or a UrlLoaderError.
   */
  Result fromUri(std::string_view uri);

private:
  /// Loader used for decoding the data URL or fetching the external resources.
  UrlLoader urlLoader_;
};

}  // namespace donner::svg
