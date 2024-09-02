#pragma once
/// @file

#include <span>
#include <string>
#include <vector>

namespace donner::svg {

/**
 * Utility class for saving images to disk and loading images from external resources or base64 data strings.
 */
class RendererImageIO {
public:
  /**
   * Write raw RGBA pixel data to a PNG file.
   *
   * @param filename Filename to save to.
   * @param rgbaPixels Span containing RGBA-ordered pixel data.
   * @param width Width of the image.
   * @param height Height of the image.
   * @param strideInPixels Stride in pixels. Defaults to 0, which assumes a stride of width.
   * @returns true if the image was written successfully.
   */
  static bool writeRgbaPixelsToPngFile(const char* filename, std::span<const uint8_t> rgbaPixels,
                                       int width, int height, size_t strideInPixels = 0);

  /**
   * Fetch external resource from a given URL.
   *
   * @param url URL of the external resource.
   * @returns A vector containing the fetched data.
   */
  static std::vector<uint8_t> fetchExternalResource(const std::string& url);

  /**
   * Decode base64 data string.
   *
   * @param base64String Base64 encoded string.
   * @returns A vector containing the decoded data.
   */
  static std::vector<uint8_t> decodeBase64Data(const std::string& base64String);

  /**
   * Load image from URL or base64 string.
   *
   * @param source URL or base64 string of the image.
   * @param width Output parameter for the width of the image.
   * @param height Output parameter for the height of the image.
   * @returns A vector containing the loaded image data.
   */
  static std::vector<uint8_t> loadImage(const std::string& source, int& width, int& height);
};

}  // namespace donner::svg
