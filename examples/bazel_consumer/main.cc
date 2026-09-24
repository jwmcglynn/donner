#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

int main() {
  constexpr std::string_view kSvg = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
      <rect id="swatch" width="8" height="8" fill="#d33" />
    </svg>
  )svg";

  donner::ParseWarningSink warnings;
  donner::ParseResult<donner::svg::SVGDocument> maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvg, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "SVG parse failed: " << maybeDocument.error() << '\n';
    return EXIT_FAILURE;
  }

  donner::svg::SVGDocument document = std::move(maybeDocument.result());
  if (!document.querySelector("#swatch").has_value()) {
    std::cerr << "Selector lookup failed: expected #swatch in the parsed SVG\n";
    return EXIT_FAILURE;
  }

  donner::svg::Renderer renderer;
  renderer.draw(document);

  if (renderer.width() != 8 || renderer.height() != 8) {
    std::cerr << "Rendered dimensions: " << renderer.width() << "x" << renderer.height()
              << "; expected 8x8\n";
    return EXIT_FAILURE;
  }
  const donner::svg::RendererBitmap bitmap = renderer.takeSnapshot();
  // The last row needs eight pixels, not a full row of stride padding.
  if (bitmap.empty() || bitmap.dimensions.x != 8 || bitmap.dimensions.y != 8 ||
      bitmap.rowBytes < 8 * 4 || bitmap.pixels.size() < 8 * 4 ||
      bitmap.rowBytes > (bitmap.pixels.size() - 8 * 4) / 7) {
    std::cerr << "Renderer did not return a complete 8x8 RGBA snapshot: " << bitmap.dimensions.x
              << "x" << bitmap.dimensions.y << ", rowBytes=" << bitmap.rowBytes
              << ", pixels=" << bitmap.pixels.size() << '\n';
    return EXIT_FAILURE;
  }

  const std::size_t offset = 4 * bitmap.rowBytes + 4 * 4;
  const std::array<std::uint8_t, 4> center = {bitmap.pixels[offset], bitmap.pixels[offset + 1],
                                              bitmap.pixels[offset + 2], bitmap.pixels[offset + 3]};
  constexpr std::array<std::uint8_t, 4> kExpectedCenter = {0xdd, 0x33, 0x33, 0xff};
  if (center != kExpectedCenter) {
    std::cerr << "Rendered center pixel: (" << static_cast<int>(center[0]) << ", "
              << static_cast<int>(center[1]) << ", " << static_cast<int>(center[2]) << ", "
              << static_cast<int>(center[3]) << "); expected opaque #d33\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
