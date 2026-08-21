/**
 * @example svg_to_png.cc Render SVG to PNG
 * @details This example demonstrates how to parse an SVG file and render it to a PNG file using
 * the active rendering backend.
 *
 * To run:
 *
 * ```sh
 * bazel run //examples:svg_to_png -- donner_splash.svg
 * ```
 *
 * The output is saved to "output.png" in the current working directory.
 */

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

#include "donner/base/FileUtils.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/TerminalEscape.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

/**
 * Main function, usage: svg_to_png <filename>
 */
int main(int argc, char* argv[]) {
  // When launched via `bazel run`, change to the user's original working
  // directory so that relative paths resolve naturally.
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: svg_to_png <filename>\n";
    return 1;
  }

  //! [load_file]
  auto fileResult = ReadFileBounded(argv[1], SVGParser::kDefaultMaximumInputSize);
  const auto* fileData = std::get_if<std::string>(&fileResult);
  if (fileData == nullptr) {
    std::cerr << FileReadErrorMessage(std::get<FileReadError>(fileResult)) << ": "
              << EscapeTerminalText(argv[1]) << "\n";
    return 1;  // Return an error code from main.
  }
  //! [load_file]

  // Parse the SVG. Note that the lifetime of the vector must be longer than the returned
  // SVGDocument, since it is referenced internally.

  //! [parse]
  SVGParser::Options options;
  // Allow data-name attributes without generating a warning.
  options.disableUserAttributes = false;

  ParseWarningSink warnings;
  // warnings and options are optional, call ParseSVG(fileData) to use defaults and ignore warnings.
  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(*fileData, warnings, options);
  //! [parse]

  //! [handle_errors]
  // ParseResult either contains an SVGDocument or an error.
  if (maybeDocument.hasError()) {
    std::ostringstream diagnostic;
    diagnostic << maybeDocument.error();
    std::cerr << "Parse Error: " << EscapeTerminalText(diagnostic.str()) << "\n";
    return 1;  // Return an error code from main.
  }

  std::cout << "Parsed successfully.\n";

  if (warnings.hasWarnings()) {
    std::cout << "Warnings:\n";
    for (const ParseDiagnostic& w : warnings.warnings()) {
      std::ostringstream diagnostic;
      diagnostic << w;
      std::cout << "  " << EscapeTerminalText(diagnostic.str()) << "\n";
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
  Renderer renderer;
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
