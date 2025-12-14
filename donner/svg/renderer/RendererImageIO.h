#pragma once
/// @file

#include <cstdint>
#include <span>

namespace donner::svg {

/**
 * Utility class for saving images to disk.
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
   * Write raw RGBA pixel data to a PNG in memory.
   *
   * @param rgbaPixels Span containing RGBA-ordered pixel data.
   * @param width Width of the image.
   * @param height Height of the image.
   * @param strideInPixels Stride in pixels. Defaults to 0, which assumes a stride of width.
   * @returns Vector containing the PNG-encoded data.
   */
  static std::vector<uint8_t> writeRgbaPixelsToPngMemory(std::span<const uint8_t> rgbaPixels,
                                                          int width, int height,
                                                          size_t strideInPixels = 0);
};

}  // namespace donner::svg
