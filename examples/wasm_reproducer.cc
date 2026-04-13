/**
 * @file wasm_reproducer.cc
 * @brief Minimal reproducer to verify RenderingInstanceComponent creation on WASM.
 *
 * Parses an SVG with rect, circle, and line elements, instantiates the render tree,
 * and counts how many entities have RenderingInstanceComponent. Expected: 3.
 */

#include <cstdlib>
#include <iostream>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

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

  // Instantiate the render tree (same as what the renderer does internally).
  Registry& registry = document.registry();
  if (!registry.ctx().contains<components::RenderingContext>()) {
    registry.ctx().emplace<components::RenderingContext>(registry);
  }

  ParseWarningSink renderWarningSink;
  registry.ctx().get<components::RenderingContext>().instantiateRenderTree(false, renderWarningSink);

  // Count entities with RenderingInstanceComponent.
  int count = 0;
  auto view = registry.view<components::RenderingInstanceComponent>();
  for ([[maybe_unused]] auto entity : view) {
    ++count;
  }

  std::cout << "RenderingInstanceComponent count: " << count << "\n";
  if (count >= 3) {
    std::cout << "PASS\n";
    return 0;
  } else {
    std::cout << "FAIL (expected >= 3, got " << count << ")\n";
    return 1;
  }
}
