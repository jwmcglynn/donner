#include <cstdlib>
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
    return EXIT_FAILURE;
  }

  donner::svg::SVGDocument document = std::move(maybeDocument.result());
  if (!document.querySelector("#swatch").has_value()) {
    return EXIT_FAILURE;
  }

  donner::svg::Renderer renderer;
  renderer.draw(document);

  return renderer.width() == 8 && renderer.height() == 8 ? EXIT_SUCCESS : EXIT_FAILURE;
}
