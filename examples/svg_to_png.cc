/**
 * @example svg_to_png.cc Render SVG to PNG
 * @details This example demonstrates how to parse an SVG file and render it to a PNG file using the
 * Skia rendering backend.
 *
 * To run:
 *
 * ```sh
 * bazel run --run_under="cd $PWD &&" //examples:svg_to_png -- donner_splash.svg
 * ```
 *
 * The output is saved to "output.png" in the current working directory.
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererSkia.h"

using namespace donner::base;
using namespace donner::base::parser;
using namespace donner::svg;
using namespace donner::svg::parser;

/**
 * Main function, usage: svg_to_png <filename>
 */
int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: svg_to_png <filename>\n";
    return 1;
  }

  //! [load_file]
  // Load the file and store it in a mutable std::vector<char>.
  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    std::abort();
  }

  std::string fileData;
  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);

  fileData.resize(fileLength);
  file.read(fileData.data(), static_cast<std::streamsize>(fileLength));
  //! [load_file]

  // Parse the SVG. Note that the lifetime of the vector must be longer than the returned
  // SVGDocument, since it is referenced internally.

  //! [parse]
  SVGParser::Options options;
  // Allow data-name attributes without generating a warning.
  options.disableUserAttributes = false;

  std::vector<ParseError> warnings;
  // warnings and options are optional, call ParseSVG(fileData) to use defaults and ignore warnings.
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData, &warnings, options);
  //! [parse]

  //! [handle_errors]
  // ParseResult either contains an SVGDocument or an error.
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    std::abort();
  }

  std::cout << "Parsed successfully.\n";

  if (!warnings.empty()) {
    std::cout << "Warnings:\n";
    for (ParseError& w : warnings) {
      std::cout << "  " << w << "\n";
    }
  }

  SVGDocument document = std::move(maybeDocument.result());
  //! [handle_errors]

  //! [set_canvas_size]
  // Setting the canvas size is equivalent to resizing a browser window. Some SVGs may scale to fit,
  // other ones may only render at their base size. To auto-size, either omit this call or invoke
  // useAutomaticCanvasSize().
  document.setCanvasSize(800, 600);
  //! [set_canvas_size]

  //! [render]
  // Draw the document, store the image in-memory.
  RendererSkia renderer;
  renderer.draw(document);

  std::cout << "Final size: " << renderer.width() << "x" << renderer.height() << "\n";

  // Then save it out using the save API.
  if (renderer.save("output.png")) {
    std::cout << "Saved to file: " << std::filesystem::absolute("output.png") << "\n";
    return 0;
  } else {
    std::cerr << "Failed to save to file: " << std::filesystem::absolute("output.png") << "\n";
    return 1;
  }
  //! [render]
}
