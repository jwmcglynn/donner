/**
 * @example svg_interactivity.cc Hit testing and element queries
 * @details This example demonstrates how to use DonnerController for hit testing, element-from-point
 * queries, and bounding box retrieval. It loads an SVG, queries elements at specific coordinates,
 * and prints their tag names and bounds.
 *
 * To run:
 *
 * ```sh
 * bazel run --run_under="cd $PWD &&" //examples:svg_interactivity -- input.svg
 * ```
 */

#include <fstream>
#include <iostream>

#include "donner/svg/DonnerController.h"
#include "donner/svg/SVG.h"

int main(int argc, char* argv[]) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  if (argc != 2) {
    std::cerr << "USAGE: svg_interactivity <filename>\n";
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 1;
  }

  std::string fileData;
  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);
  fileData.resize(fileLength);
  file.read(fileData.data(), static_cast<std::streamsize>(fileLength));

  SVGParser::Options options;
  options.enableExperimental = true;

  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData, nullptr, options);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(800, 600);

  // Create a controller for hit testing and spatial queries.
  DonnerController controller(document);

  // Query the center of the canvas.
  const Vector2d center(400.0, 300.0);

  std::cout << "Querying elements at (" << center.x << ", " << center.y << "):\n\n";

  // Find the topmost element at the center.
  if (auto element = controller.findIntersecting(center)) {
    std::cout << "Topmost element: <" << element->tagName() << ">";
    if (auto id = element->cast<SVGElement>().getAttribute("id")) {
      std::cout << " id=\"" << *id << "\"";
    }
    std::cout << "\n";

    // Get its world-space bounding box.
    if (auto bounds = controller.getWorldBounds(element->cast<SVGElement>())) {
      std::cout << "  Bounds: (" << bounds->topLeft.x << ", " << bounds->topLeft.y << ") - ("
                << bounds->bottomRight.x << ", " << bounds->bottomRight.y << ")\n";
    }
  } else {
    std::cout << "No element found at center.\n";
  }

  // Find all elements at the center (front-to-back order).
  auto allElements = controller.findAllIntersecting(center);
  std::cout << "\nAll elements at center (" << allElements.size() << " total):\n";
  for (const auto& element : allElements) {
    std::cout << "  <" << element.tagName() << ">";
    if (auto id = element.getAttribute("id")) {
      std::cout << " id=\"" << *id << "\"";
    }
    std::cout << "\n";
  }

  // Demonstrate querySelector.
  std::cout << "\nQuerySelector examples:\n";
  if (auto element = document.querySelector("rect")) {
    std::cout << "  Found <rect>";
    if (auto id = element->getAttribute("id")) {
      std::cout << " id=\"" << *id << "\"";
    }
    std::cout << "\n";
  }

  if (auto element = document.querySelector("circle")) {
    std::cout << "  Found <circle>";
    if (auto id = element->getAttribute("id")) {
      std::cout << " id=\"" << *id << "\"";
    }
    std::cout << "\n";
  }

  return 0;
}
