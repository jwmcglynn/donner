/**
 * @example svg_quickstart.cc Parse, edit and render an SVG
 * @details Parses an SVG from memory, finds an element by its `data-*` attribute, changes its
 * style, and renders the document with the build's default renderer, saving `output.png` in the
 * current working directory.
 *
 * ```sh
 * bazel run //examples:svg_quickstart
 * ```
 */

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
//! [includes]
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
//! [includes]

int main() {
  // Under `bazel run`, save output.png in the caller's working directory.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  //! [quickstart]
  constexpr std::string_view kSvg = R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <rect data-role="status" width="64" height="64" fill="gray" />
    </svg>)";

  donner::ParseWarningSink warnings;
  donner::ParseResult<donner::svg::SVGDocument> maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvg, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }

  donner::svg::SVGDocument document = std::move(maybeDocument.result());

  // Custom data-* attributes are kept by default, so attribute selectors match them.
  std::optional<donner::svg::SVGElement> status = document.querySelector("[data-role=status]");
  if (!status) {
    std::cerr << "No element has data-role=status\n";
    return 1;
  }
  status->setStyle("fill: green");

  donner::svg::Renderer renderer;
  renderer.draw(document);
  if (!renderer.save("output.png")) {
    std::cerr << "Could not save output.png\n";
    return 1;
  }
  //! [quickstart]

  //! [snapshot]
  donner::svg::RendererBitmap snapshot = renderer.takeSnapshot();
  std::cout << "Rendered " << snapshot.dimensions.x << "x" << snapshot.dimensions.y << " pixels\n";
  //! [snapshot]
  return 0;
}
