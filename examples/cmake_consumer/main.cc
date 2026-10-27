#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/base/PackagedResource.h"
#include "donner/svg/SVGTextElement.h"
#endif

int main() {
#ifdef DONNER_TEXT_ENABLED
  std::string resourceError;
  if (!donner::PreparePackagedResources(&resourceError)) {
    std::fprintf(stderr, "Packaged resources unavailable: %s\n", resourceError.c_str());
    return EXIT_FAILURE;
  }
#endif
  constexpr std::string_view kSvg = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">
      <rect id="swatch" width="32" height="32" fill="#d33" />
      <text id="caption" x="2" y="24" font-family="UninstalledFixtureFamily" font-size="16">Hi</text>
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

#ifdef DONNER_TEXT_ENABLED
  auto caption = document.querySelector("#caption");
  if (!caption || caption->cast<donner::svg::SVGTextElement>().getComputedTextLength() <= 0) {
    return EXIT_FAILURE;
  }
#endif
  return renderer.width() == 32 && renderer.height() == 32 ? EXIT_SUCCESS : EXIT_FAILURE;
}
