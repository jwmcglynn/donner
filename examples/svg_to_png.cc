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

using donner::base::parser::ParseError;
using donner::base::parser::ParseResult;

/**
 * Main function, usage: svg_to_png <filename>
 */
int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: svg_to_png <filename>\n";
    return 1;
  }

  //! [svg_to_png load_file]
  // Load the file and store it in a mutable std::vector<char>.
  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 2;
  }

  donner::svg::parser::XMLParser::InputBuffer fileData;
  fileData.loadFromStream(file);
  //! [svg_to_png load_file]

  // Parse the SVG. Note that the lifetime of the vector must be longer than the returned
  // SVGDocument, since it is referenced internally.

  //! [svg_to_png parse]
  // The warnings list is optional, call ParseSVG(fileData) to ignore warnings.
  std::vector<ParseError> warnings;
  ParseResult<donner::svg::SVGDocument> maybeResult =
      donner::svg::parser::XMLParser::ParseSVG(fileData, &warnings);
  //! [svg_to_png parse]

  //! [svg_to_png handle_errors]
  // ParseResult either contains an SVGDocument or an error.
  if (maybeResult.hasError()) {
    const ParseError& e = maybeResult.error();
    std::cerr << "Parse Error: " << e << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  if (!warnings.empty()) {
    std::cout << "Warnings:\n";
    for (ParseError& w : warnings) {
      std::cout << "  " << w << "\n";
    }
  }

  donner::svg::SVGDocument document = std::move(maybeResult.result());
  //! [svg_to_png handle_errors]

  //! [svg_to_png set_canvas_size]
  // Setting the canvas size is equivalent to resizing a browser window. Some SVGs may scale to fit,
  // other ones may only render at their base size. To auto-size, either omit this call or invoke
  // useAutomaticCanvasSize().
  document.setCanvasSize(800, 600);
  //! [svg_to_png set_canvas_size]

  //! [svg_to_png render]
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
  //! [svg_to_png render]
}
