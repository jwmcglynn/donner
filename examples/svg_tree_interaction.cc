#include <iostream>
#include <vector>

#include "src/base/utils.h"
#include "src/svg/svg.h"
#include "src/svg/svg_path_element.h"

namespace {

/**
 * A helper that converts a string into a mutable string that is suitable for use with Donner's
 * XMLParser.
 */
struct MutableString : std::vector<char> {
  explicit MutableString(std::string_view str) {
    // Reserve enough space for the string, and an extra byte for the NUL ('\0') terminator if
    // required.
    const bool hasNul = str.ends_with('\0');
    reserve(str.size() + (hasNul ? 0 : 1));
    std::copy(str.begin(), str.end(), std::back_inserter(*this));
    if (!hasNul) {
      push_back('\0');
    }
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  // This is the base SVG we are loading, a simple path containing a line.
  MutableString svgContents(R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 10 10">
      <path d="M 1 1 L 4 5" stroke="blue" />
    </svg>
  )");

  // Call ParseSVG to load the SVG file, not that this modifies the original string, and the
  // underlying string must remain valid as long as the SVGDocument is in use.
  donner::ParseResult<donner::svg::SVGDocument> maybeResult =
      donner::svg::XMLParser::ParseSVG(svgContents);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << "\n";
    return 1;
  }

  donner::svg::SVGDocument document = std::move(maybeResult.result());

  // querySelector supports standard CSS selectors, anything that's valid when defining a CSS rule
  // works here too, for example querySelector("svg > path[fill='blue']") is also valid and will
  // match the same element.
  auto maybePath = document.svgElement().querySelector("path");
  UTILS_RELEASE_ASSERT_MSG(maybePath, "Failed to find path element");

  // The result of querySelector is a generic SVGElement, but we know it's a path, so we can cast
  // it. If the cast fails, an assertion will be triggered.
  donner::svg::SVGPathElement path = maybePath->cast<donner::svg::SVGPathElement>();
  if (std::optional<donner::svg::PathSpline> spline = path.computedSpline()) {
    std::cout << "Path: " << *spline << "\n";
    std::cout << "Length: " << spline->pathLength() << " userspace units\n";
  } else {
    std::cout << "Path is empty\n";
  }

  // We could also use \ref SVGElement::isa<>() to check if the element is a path, and then cast it.
  assert(maybePath->isa<donner::svg::SVGPathElement>());

  // Set styles, note that these combine together and do not replace.
  path.setStyle("fill: red");
  path.setStyle("stroke: white");

  // Get the parsed, cascaded style for this element and output it to the console.
  std::cout << "Computed style: " << path.getComputedStyle() << "\n";

  return 0;
}
