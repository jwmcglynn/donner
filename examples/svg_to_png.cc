#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/svg.h"

using donner::base::parser::ParseError;
using donner::base::parser::ParseResult;

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: svg_to_png <filename>\n";
    return 1;
  }

  // Load the file and store it in a mutable std::vector<char>.
  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 2;
  }

  file.seekg(0, std::ios::end);
  const std::streamsize fileLength = file.tellg();
  file.seekg(0);

  std::vector<char> fileData(fileLength + 1);
  file.read(fileData.data(), fileLength);

  // Parse the SVG. Note that the lifetime of the vector must be longer than the returned
  // SVGDocument, since it is referenced internally.

  // The warnings list is optional, call ParseSVG(fileData) to ignore warnings.
  std::vector<ParseError> warnings;
  ParseResult<donner::svg::SVGDocument> maybeResult =
      donner::svg::parser::XMLParser::ParseSVG(fileData, &warnings);

  // ParseResult either contains an SVGDocument or an error.
  if (maybeResult.hasError()) {
    const ParseError& e = maybeResult.error();
    std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  if (!warnings.empty()) {
    std::cout << "Warnings:\n";
    for (ParseError& w : warnings) {
      std::cout << "  " << w.line << ":" << w.offset << ": " << w.reason << "\n";
    }
  }

  donner::svg::SVGDocument document = std::move(maybeResult.result());
  // Setting the canvas size is equivalent to resizing a browser window. Some SVGs may scale to fit,
  // other ones may only render at their base size. To auto-size, either omit this call or invoke
  // useAutomaticCanvasSize().
  document.setCanvasSize(800, 600);

  // Draw the document, store the image in-memory.
  donner::svg::RendererSkia renderer;
  renderer.draw(document);

  std::cout << "Final size: " << renderer.width() << "x" << renderer.height() << "\n";

  // Then save it out using the save API.
  constexpr const char* kOutputFilename = "output.png";
  if (renderer.save(kOutputFilename)) {
    std::cout << "Saved to file: " << std::filesystem::absolute(kOutputFilename) << "\n";
    return 0;
  } else {
    std::cerr << "Failed to save to file: " << std::filesystem::absolute(kOutputFilename) << "\n";
    return 1;
  }
}
