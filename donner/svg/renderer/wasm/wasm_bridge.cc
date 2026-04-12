/**
 * @file
 * WASM bridge for Donner SVG renderer.
 *
 * Provides a C API for parsing SVG text and rendering to RGBA pixel buffers,
 * suitable for use from JavaScript via Emscripten's ccall/cwrap.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

namespace {

/// Stores the last error message for retrieval via donner_get_last_error().
std::string gLastError;

}  // namespace

extern "C" {

void donner_init() {
  // No global initialization required; reserved for future use.
}

uint8_t* donner_render_svg(const char* svgText, int width, int height) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  gLastError.clear();

  if (svgText == nullptr) {
    gLastError = "SVG text is null";
    return nullptr;
  }

  if (width <= 0 || height <= 0) {
    gLastError = "Width and height must be positive";
    return nullptr;
  }

  // Parse the SVG document.
  ParseWarningSink warnings;
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svgText, warnings);

  if (maybeDocument.hasError()) {
    gLastError = "Parse error: " + maybeDocument.error().reason.str();
    return nullptr;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(width, height);

  // Render using the default backend (tiny-skia).
  Renderer renderer;
  renderer.draw(document);

  // Take a snapshot to get RGBA pixel data.
  RendererBitmap bitmap = renderer.takeSnapshot();
  if (bitmap.empty()) {
    gLastError = "Rendering produced an empty bitmap";
    return nullptr;
  }

  const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  // NOLINTNEXTLINE: malloc is required for Emscripten interop — JS frees via donner_free_pixels.
  auto* pixels = static_cast<uint8_t*>(std::malloc(expectedBytes));
  if (pixels == nullptr) {
    gLastError = "Failed to allocate pixel buffer";
    return nullptr;
  }

  // Copy rows, handling potential stride differences between the renderer's
  // rowBytes and the tightly-packed output expected by the caller.
  const size_t dstRowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const size_t srcOffset = static_cast<size_t>(y) * bitmap.rowBytes;
    const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
    if (srcOffset + dstRowBytes > bitmap.pixels.size()) {
      break;
    }
    std::memcpy(pixels + dstOffset, bitmap.pixels.data() + srcOffset, dstRowBytes);
  }

  return pixels;
}

void donner_free_pixels(uint8_t* pixels) {
  std::free(pixels);  // NOLINT: matches malloc in donner_render_svg.
}

const char* donner_get_last_error() {
  return gLastError.c_str();
}

}  // extern "C"
