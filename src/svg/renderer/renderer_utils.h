#pragma once

#include <span>
#include <string_view>

#include "src/svg/svg_document.h"

namespace donner::svg {

class RendererUtils {
public:
  /**
   * Prepare the document for rendering, instantiating computed components and the rendering tree.
   *
   * @param document Document to prepare.
   * @param verbose If true, enable verbose logging.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  static void prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                          std::vector<ParseError>* outWarnings = nullptr);

  /**
   * Write raw RGBA pixel data to a PNG file.
   *
   * @param filename Filename to save to.
   * @param pixels Span containing RGBA-ordered pixel data.
   * @param width Width of the image.
   * @param height Height of the image.
   * @param strideInPixels Stride in pixels. Defaults to 0, which assumes a stride of width.
   * @returns true if the image was written successfully.
   */
  static bool writeRgbaPixelsToPngFile(const char* filename, std::span<const uint8_t> rgbaPixels,
                                       int width, int height, size_t strideInPixels = 0);
};

}  // namespace donner::svg
