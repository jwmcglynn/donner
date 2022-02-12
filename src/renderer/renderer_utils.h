#pragma once

#include <span>
#include <string_view>

#include "src/svg/svg_document.h"

namespace donner {

class RendererUtils {
public:
  static void prepareDocumentForRendering(svg::SVGDocument& document, Vector2d defaultSize);

  /**
   * Write raw RGBA pixel data to a PNG file.
   *
   * @param filename Filename to save to.
   * @param pixels Span containing RGBA-ordered pixel data.
   * @param width Width of the image.
   * @param height Height of the image.
   * @returns true if the image was written successfully.
   */
  static bool writeRgbaPixelsToPngFile(const char* filename, std::span<const uint8_t> rgbaPixels,
                                       int width, int height);
};

}  // namespace donner
