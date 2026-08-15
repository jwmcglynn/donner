/**
 * @file
 * WASM bridge for Donner SVG renderer.
 *
 * Provides a C API for parsing SVG text and rendering to RGBA pixel buffers,
 * suitable for use from JavaScript via Emscripten's ccall/cwrap.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/wasm/WasmBridgeUtils.h"

namespace {

/// Stores the last error message for retrieval via donner_get_last_error().
std::string gLastError;

constexpr size_t kMaximumPixelBufferSize = 64 * 1024 * 1024;

uint8_t* RenderSvg(std::string_view svgText, int width, int height) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  if (svgText.size() > SVGParser::kDefaultMaximumInputSize) {
    gLastError = "SVG exceeds maximum input size";
    return nullptr;
  }

  if (width <= 0 || height <= 0) {
    gLastError = "Width and height must be positive";
    return nullptr;
  }

  constexpr int kMaxDimension = 8192;
  if (width > kMaxDimension || height > kMaxDimension) {
    gLastError = "Dimensions exceed maximum (" + std::to_string(kMaxDimension) + "x" +
                 std::to_string(kMaxDimension) + ")";
    return nullptr;
  }

  const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (area > kMaximumPixelBufferSize / 4) {
    gLastError = "Pixel buffer exceeds maximum size";
    return nullptr;
  }
  const size_t expectedBytes = area * 4;

  ParseWarningSink warnings;
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svgText, warnings);
  if (maybeDocument.hasError()) {
    gLastError = "Parse error: " + maybeDocument.error().reason.str();
    return nullptr;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(width, height);

  Renderer renderer;
  renderer.draw(document);

  RendererBitmap bitmap = renderer.takeSnapshot();
  const size_t dstRowBytes = static_cast<size_t>(width) * 4;
  if (bitmap.empty() || bitmap.dimensions != Vector2i(width, height) ||
      bitmap.rowBytes < dstRowBytes || bitmap.rowBytes > bitmap.pixels.size() ||
      static_cast<size_t>(height) > bitmap.pixels.size() / bitmap.rowBytes) {
    gLastError = "Rendering produced an invalid bitmap";
    return nullptr;
  }

  // NOLINTNEXTLINE: malloc is required for Emscripten interop - JS frees via donner_free_pixels.
  auto* pixels = static_cast<uint8_t*>(std::malloc(expectedBytes));
  if (pixels == nullptr) {
    gLastError = "Failed to allocate pixel buffer";
    return nullptr;
  }

  for (int y = 0; y < height; ++y) {
    const size_t srcOffset = static_cast<size_t>(y) * bitmap.rowBytes;
    const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
    std::memcpy(pixels + dstOffset, bitmap.pixels.data() + srcOffset, dstRowBytes);
  }
  return pixels;
}

}  // namespace

extern "C" {

void donner_init() {
  // No global initialization required; reserved for future use.
}

uint8_t* donner_render_svg(const char* svgText, int width, int height) {
  gLastError.clear();

  if (svgText == nullptr) {
    gLastError = "SVG text is null";
    return nullptr;
  }

  const auto length = donner::svg::wasm::BoundedCStringLength(
      svgText, donner::svg::parser::SVGParser::kDefaultMaximumInputSize);
  if (!length.has_value()) {
    gLastError = "SVG text is outside WebAssembly memory";
    return nullptr;
  }
  return RenderSvg(std::string_view(svgText, *length), width, height);
}

uint8_t* donner_render_svg_len(const char* svgText, size_t svgTextSize, int width, int height) {
  gLastError.clear();
  if (svgText == nullptr) {
    gLastError = "SVG text is null";
    return nullptr;
  }
  if (svgTextSize > donner::svg::parser::SVGParser::kDefaultMaximumInputSize) {
    gLastError = "SVG exceeds maximum input size";
    return nullptr;
  }
  if (!donner::svg::wasm::IsValidInputRange(svgText, svgTextSize)) {
    gLastError = "SVG text is outside WebAssembly memory";
    return nullptr;
  }
  return RenderSvg(std::string_view(svgText, svgTextSize), width, height);
}

void donner_free_pixels(uint8_t* pixels) {
  std::free(pixels);  // NOLINT: matches malloc in donner_render_svg.
}

const char* donner_get_last_error() {
  return gLastError.c_str();
}

}  // extern "C"
