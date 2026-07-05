/**
 * @file wasm_reproducer.cc
 * @brief Minimal reproducer to verify SVG rendering on WASM.
 *
 * Parses an SVG with rect, circle, and line elements, renders it through the public renderer, and
 * verifies that the expected canvas dimensions are produced.
 */

#include <cstdlib>
#include <iostream>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

int main() {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  const char* svgSource =
      "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
      "<rect fill='red' width='80' height='80'/>"
      "<circle cx='40' cy='40' r='20' fill='blue'/>"
      "<line x1='0' y1='0' x2='100' y2='100' stroke='black'/>"
      "</svg>";

  ParseWarningSink warningSink;
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(svgSource, warningSink);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(100, 100);

  Renderer renderer;
  renderer.draw(document);

  std::cout << "Rendered canvas size: " << renderer.width() << "x" << renderer.height() << "\n";
  if (renderer.width() == 100 && renderer.height() == 100) {
    std::cout << "PASS\n";
    return 0;
  } else {
    std::cout << "FAIL (expected 100x100)\n";
    return 1;
  }
}
