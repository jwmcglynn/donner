/**
 * @file
 * WASM bridge for Donner SVG renderer — Geode (WebGPU) backend.
 *
 * Provides the same C ABI as `wasm_bridge.cc` but uses `RendererGeode`
 * directly so the rendering path goes through the browser's native WebGPU
 * implementation. Requires `-sUSE_WEBGPU=1` and `-sASYNCIFY=1` in the
 * Emscripten link flags (ASYNCIFY is needed because `GeodeDevice` creation
 * and `takeSnapshot()` readback perform synchronous waits that must yield
 * to the browser event loop).
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererGeode.h"

namespace {

/// Stores the last error message for retrieval via donner_get_last_error().
std::string gLastError;

}  // namespace

extern "C" {

void donner_geode_init() {
  // Reserved for future use. WebGPU device creation happens lazily on
  // first render because the browser's adapter/device request is async
  // and requires ASYNCIFY yield.
}

uint8_t* donner_geode_render_svg(const char* svgText, int width, int height) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  std::cerr << "[wasm] render_svg entry w=" << width << " h=" << height << std::endl;

  gLastError.clear();

  if (svgText == nullptr) {
    gLastError = "SVG text is null";
    return nullptr;
  }

  if (width <= 0 || height <= 0) {
    gLastError = "Width and height must be positive";
    return nullptr;
  }

  constexpr int kMaxDimension = 4096;
  if (width > kMaxDimension || height > kMaxDimension) {
    gLastError = "Dimensions exceed maximum (" + std::to_string(kMaxDimension) + "x" +
                 std::to_string(kMaxDimension) + ")";
    return nullptr;
  }

  const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (area > SIZE_MAX / 4) {
    gLastError = "Pixel buffer too large";
    return nullptr;
  }
  const size_t expectedBytes = area * 4;

  // Parse the SVG document.
  std::cerr << "[wasm] parsing SVG (" << std::strlen(svgText) << " bytes)" << std::endl;
  ParseWarningSink warnings;
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svgText, warnings);

  if (maybeDocument.hasError()) {
    gLastError = "Parse error: " + maybeDocument.error().reason.str();
    return nullptr;
  }
  std::cerr << "[wasm] parsed OK" << std::endl;

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(width, height);

  // Render using the Geode (WebGPU) backend. Under Emscripten the
  // RendererGeode constructor calls GeodeDevice::CreateHeadless() which
  // requests an adapter and device — these are async in the browser and
  // require ASYNCIFY to complete synchronously.
  std::cerr << "[wasm] constructing RendererGeode (will acquire WebGPU device)" << std::endl;
  RendererGeode renderer(/*verbose=*/true);
  std::cerr << "[wasm] RendererGeode constructed; calling draw()" << std::endl;
  renderer.draw(document);
  std::cerr << "[wasm] draw() returned; calling takeSnapshot()" << std::endl;

  // Take a snapshot to get RGBA pixel data. This submits a GPU→CPU copy
  // and maps the readback buffer, which again requires ASYNCIFY yield.
  RendererBitmap bitmap = renderer.takeSnapshot();
  std::cerr << "[wasm] takeSnapshot() returned; pixels.size=" << bitmap.pixels.size()
            << " rowBytes=" << bitmap.rowBytes << std::endl;
  if (bitmap.empty()) {
    gLastError = "Rendering produced an empty bitmap";
    return nullptr;
  }

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
  std::free(pixels);  // NOLINT: matches malloc in donner_geode_render_svg.
}

const char* donner_get_last_error() {
  return gLastError.c_str();
}

}  // extern "C"
