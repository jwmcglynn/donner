/**
 * @example svg_filter_interaction.cc SVG Filter DOM Manipulation
 *
 * Demonstrates how to inspect and modify SVG filters through the Donner DOM. Parses an SVG
 * containing an `<feGaussianBlur>` filter, finds the blur primitive, changes its
 * `stdDeviation`, swaps the filter on a shape, then renders to PNG.
 *
 * ```sh
 * bazel run //examples:svg_filter_interaction -- output.png
 * ```
 */

#include <iostream>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
#include "donner/svg/renderer/Renderer.h"

using donner::svg::SVGDocument;
using donner::svg::SVGElement;
using donner::svg::SVGFEGaussianBlurElement;

int main(int argc, char* argv[]) {
  //! [filter_svg_source]
  // A minimal document with a drop-shadow-style Gaussian blur applied to a rectangle.
  const std::string_view kSvgSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160">
      <defs>
        <filter id="Blur" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="2" />
        </filter>
        <filter id="Heavy" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="8" />
        </filter>
      </defs>
      <rect x="40" y="30" width="100" height="100" rx="12"
            fill="#5aa9ff" stroke="#1f5a8a" stroke-width="3"
            filter="url(#Blur)" />
      <rect x="180" y="30" width="100" height="100" rx="12"
            fill="#e0a63a" stroke="#7a5a1a" stroke-width="3"
            filter="url(#Blur)" />
    </svg>
  )svg");
  //! [filter_svg_source]

  donner::ParseWarningSink warnings;
  auto maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvgSource, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse error: " << maybeDocument.error() << "\n";
    return 1;
  }
  SVGDocument document = std::move(maybeDocument.result());

  //! [filter_query_primitive]
  // Find the first <feGaussianBlur> in the document. Filter primitives are ordinary SVG
  // elements in the DOM and can be queried via CSS selectors just like shapes.
  std::optional<SVGElement> maybeBlur = document.querySelector("feGaussianBlur");
  UTILS_RELEASE_ASSERT_MSG(maybeBlur, "Expected an <feGaussianBlur> in the document");
  SVGFEGaussianBlurElement blur = maybeBlur->cast<SVGFEGaussianBlurElement>();

  std::cout << "Initial stdDeviation: (" << blur.stdDeviationX() << ", " << blur.stdDeviationY()
            << ")\n";
  //! [filter_query_primitive]

  //! [filter_mutate_primitive]
  // Crank the blur up. The renderer picks up the new values on the next draw — filters are
  // part of the live document, not baked at parse time.
  blur.setStdDeviation(6.0, 6.0);
  std::cout << "After mutation, stdDeviation: (" << blur.stdDeviationX() << ", "
            << blur.stdDeviationY() << ")\n";
  //! [filter_mutate_primitive]

  //! [filter_swap]
  // Swap the filter reference on the second rect. CSS presentation attributes (including
  // `filter`) can be set via setStyle(), which accepts any normal CSS declaration.
  std::optional<SVGElement> maybeSecondRect =
      document.querySelector("rect:nth-of-type(2)");
  UTILS_RELEASE_ASSERT_MSG(maybeSecondRect, "Expected a second <rect>");
  maybeSecondRect->setStyle("filter: url(#Heavy)");
  //! [filter_swap]

  //! [filter_render]
  const std::string output = argc >= 2 ? argv[1] : "filter_interaction.png";
  donner::svg::Renderer renderer;
  renderer.draw(document);
  if (!renderer.save(output.c_str())) {
    std::cerr << "Failed to save " << output << "\n";
    return 1;
  }
  std::cout << "Rendered to " << output << " (" << renderer.width() << "x" << renderer.height()
            << ")\n";
  //! [filter_render]

  return 0;
}
