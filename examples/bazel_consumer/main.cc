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
  return EXIT_SUCCESS;
}
