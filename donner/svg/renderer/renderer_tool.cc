/**
 * @file renderer_tool.cc
 *
 * # Donner SVG Renderer Tool
 *
 * Renders an `.svg` file and prints debugging information about it, such as the parsed tree and
 * warnings. Saves the output to `output.png`.
 *
 * ```
 * USAGE: renderer_tool <filename> [--quiet]
 *
 *  filename: The SVG file to render.
 *  --quiet: Do not output the parsed tree or warnings.
 * ```
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

namespace donner::svg {

/**
 * Implements a simple RAII execution time tracer.
 *
 * Usage:
 * ```
 * {
 *   Trace trace("My trace");
 *  ...
 * }
 * ```
 *
 * This will print the execution time of the code within the block.
 **/
class Trace {
public:
  /// Start a new trace with the given name.
  explicit Trace(const char* name) : name_(name) {}

  // No copy or move.
  Trace(const Trace&) = delete;
  Trace(Trace&&) = delete;
  Trace& operator=(const Trace&) = delete;
  Trace& operator=(Trace&&) = delete;

  /// Stop the trace.
  ~Trace() { stop(); }

  /**
   *
   * Explicitly stop the trace, to stop before this object is destructed. Once stopped, the trace
   * cannot be restarted.
   *
   * Example:
   * ```
   * {
   *   Trace trace("My trace");
   *   ...
   *   trace.stop();
   *   ...
   * }
   * ```
   */
  void stop() {
    if (!stopped_) {
      stopped_ = true;

      auto end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);
      std::cout << name_ << ": " << duration.count() << "ms"
                << "\n";
    }
  }

private:
  const char* name_;      //!< Trace name.
  bool stopped_ = false;  //!< True if the trace has been stopped.
  /// Trace start time.
  std::chrono::time_point<std::chrono::high_resolution_clock> start_ =
      std::chrono::high_resolution_clock::now();
};

/**
 * Dump the SVG tree to the console, starting with \p element.
 *
 * @param element The root element of the tree to dump.
 * @param depth The depth of the current element in the tree, used to control indentation. Defaults
 * to 0 for the root.
 */
void DumpTree(SVGElement element, int depth = 0) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ", " << element.entity() << ", id: '" << element.id() << "'";
  if (element.type() == ElementType::SVG) {
    if (auto viewbox = element.cast<SVGSVGElement>().viewbox()) {
      std::cout << ", viewbox: " << *viewbox;
    }
  }
  std::cout << "\n";
  for (auto elm = element.firstChild(); elm; elm = elm->nextSibling()) {
    DumpTree(elm.value(), depth + 1);
  }
}

/// Tool entry point, usage is described in the file header.
extern "C" int main(int argc, char* argv[]) {
  // Initialize the symbolizer to get a human-readable stack trace
  absl::InitializeSymbolizer(argv[0]);

  absl::FailureSignalHandlerOptions options;
  absl::InstallFailureSignalHandler(options);

  if (argc != 2 && argc != 3) {
    std::cerr << "Unexpected arg count."
              << "\n";
    std::cerr << "USAGE: renderer_tool <filename> [--quiet]"
              << "\n";
    return 1;
  }

  if (argv[1] == std::string_view("--help")) {
    std::cout << "Donner SVG Renderer tool\n";
    std::cout << "\n";
    std::cout << "USAGE: renderer_tool <filename> [--quiet]\n";
    std::cout << "\n";
    std::cout << "  filename: The SVG file to render.\n";
    std::cout << "  --quiet: Do not output the parsed tree or warnings.\n";
    std::cout << "\n";
    std::cout << "This will output the parsed tree and render the SVG to a file named 'output.png' "
                 "in the working directory\n";
    std::cout << "\n";
    return 0;
  }

  std::string filename;
  bool quiet = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::string_view("--quiet")) {
      quiet = true;
    } else if (argv[i] == std::string_view("--verbose")) {
      verbose = true;
    } else {
      filename = argv[i];
    }
  }

  if (filename.empty()) {
    std::cerr << "No filename specified.\n";
    return 1;
  }

  std::ifstream file(filename);
  if (!file) {
    std::cerr << "Could not open file " << filename << "\n";
    return 2;
  }

  parser::XMLParser::InputBuffer fileData;
  fileData.loadFromStream(file);

  std::vector<parser::ParseError> warnings;
  parser::XMLParser::Options xmlOptions;
  auto resourceLoader =
      std::make_unique<SandboxedFileResourceLoader>(std::filesystem::current_path(), filename);

  Trace traceParse("Parse");
  auto maybeResult = parser::XMLParser::ParseSVG(fileData, quiet ? nullptr : &warnings, xmlOptions,
                                                 std::move(resourceLoader));
  traceParse.stop();

  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  if (!warnings.empty()) {
    std::cout << "Warnings:\n";
    for (auto& w : warnings) {
      std::cout << "  " << w << "\n";
    }
  }

  SVGDocument document = std::move(maybeResult.result());

  if (!quiet) {
    std::cout << "Tree:\n";
    DumpTree(document.svgElement(), 0);
  }

  if (auto path1 = document.querySelector("#path1")) {
    std::cout << "Found path1\n";
    path1->setStyle("fill: red");
    path1->setStyle("stroke: white");
  }

  document.setCanvasSize(600, 600);

  {
    warnings.clear();

    // Manually call prepareDocumentForRendering so we can measure how long it takes. This is
    // normally called automatically by the Renderer.
    Trace tracePrepare("Prepare");
    RendererUtils::prepareDocumentForRendering(document, false, quiet ? nullptr : &warnings);

    if (!warnings.empty()) {
      std::cout << "Warnings:\n";
      for (auto& w : warnings) {
        std::cout << "  " << w << "\n";
      }
    }
  }

  Trace traceCreateRenderer("Create Renderer");
  RendererSkia renderer(verbose);
  traceCreateRenderer.stop();

  {
    Trace traceRender("Render");
    renderer.draw(document);
  }

  std::cout << "Final size: " << renderer.width() << "x" << renderer.height() << "\n";

  constexpr const char* kOutputFilename = "output.png";
  if (renderer.save(kOutputFilename)) {
    std::cout << "Saved to file: " << std::filesystem::absolute(kOutputFilename) << "\n";
    return 0;
  } else {
    std::cerr << "Failed to save to file: " << std::filesystem::absolute(kOutputFilename) << "\n";
    return 1;
  }
}

}  // namespace donner::svg
