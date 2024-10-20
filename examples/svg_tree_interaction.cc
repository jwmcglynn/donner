/**
 * @example svg_tree_interaction.cc SVG Tree Interaction
 *
 * Demonstrates how to interact with the SVG DOM. This example loads an SVG file, gets the
 * SVGPathElement for a path in the image, then prints metadata and modifies it.
 *
 * ```sh
 * bazel run //examples:svg_tree_interaction
 * ```
 */

#include <iostream>

#include "donner/base/Length.h"
#include "donner/base/Utils.h"
#include "donner/svg/SVG.h"
#include "donner/svg/SVGCircleElement.h"
#include "donner/svg/SVGPathElement.h"

using donner::base::parser::ParseError;
using donner::base::parser::ParseResult;

int main(int argc, char* argv[]) {
  //! [homepage_snippet]
  //! [svg_string]
  // This is the base SVG we are loading, a simple path containing a line
  donner::svg::parser::SVGParser::InputBuffer svgContents(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 10 10">
      <path d="M 1 1 L 4 5" stroke="blue" />
    </svg>
  )");
  //! [svg_string]

  //! [svg_parse]
  // Call ParseSVG to load the SVG file
  ParseResult<donner::svg::SVGDocument> maybeResult =
      donner::svg::parser::SVGParser::ParseSVG(svgContents);
  //! [svg_parse]
  //! [error_handling]
  if (maybeResult.hasError()) {
    std::cerr << "Parse Error " << maybeResult.error() << "\n";  // Includes line:column and reason
    std::abort();
    // - or - handle the error per your project's conventions
  }
  //! [error_handling]

  //! [get_path]
  donner::svg::SVGDocument document = std::move(maybeResult.result());

  // querySelector supports standard CSS selectors, anything that's valid when defining a CSS rule
  // works here too, for example querySelector("svg > path[fill='blue']") is also valid and will
  // match the same element.
  std::optional<donner::svg::SVGElement> maybePath = document.querySelector("path");
  UTILS_RELEASE_ASSERT_MSG(maybePath.has_value(), "Failed to find path element");

  // The result of querySelector is a generic SVGElement, but we know it's a path, so we can cast
  // it. If the cast fails, an assertion will be triggered.
  donner::svg::SVGPathElement path = maybePath->cast<donner::svg::SVGPathElement>();
  //! [get_path]
  if (std::optional<donner::svg::PathSpline> spline = path.computedSpline()) {
    std::cout << "Path: " << *spline << "\n";
    std::cout << "Length: " << spline->pathLength() << " userspace units\n";
  } else {
    std::cout << "Path is empty\n";
  }
  //! [homepage_snippet]

  // We could also use \ref SVGElement::isa<>() to check if the element is a path, and then cast it.
  assert(maybePath->isa<donner::svg::SVGPathElement>());

  // Set styles, note that these combine together and do not replace.
  //! [path_set_style]
  path.setStyle("fill: red");
  path.setStyle("stroke: white");

  // Get the parsed, cascaded style for this element and output it to the console.
  std::cout << "Computed style: " << path.getComputedStyle() << "\n";
  //! [path_set_style]

  //! [add_circle]
  // Add a circle to the document
  donner::svg::SVGCircleElement circle = donner::svg::SVGCircleElement::Create(document);
  circle.setCx(donner::Lengthd(5));
  circle.setCy(donner::Lengthd(5));
  circle.setR(donner::Lengthd(4));
  circle.setStyle("color: #AAA");

  document.svgElement().insertBefore(circle, path);
  //! [add_circle]

  return 0;
}
