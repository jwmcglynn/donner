#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg {

class Trace {
public:
  explicit Trace(const char* name) : name_(name) {}

  ~Trace() { stop(); }

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
  const char* name_;
  bool stopped_ = false;
  std::chrono::time_point<std::chrono::high_resolution_clock> start_ =
      std::chrono::high_resolution_clock::now();
};

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << TypeToString(element.type()) << ", " << element.entity() << ", id: '" << element.id()
            << "'";
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
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::string_view("--quiet")) {
      quiet = true;
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

  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);

  std::vector<char> fileData(fileLength + 1);
  file.read(fileData.data(), static_cast<std::streamsize>(fileLength));

  std::vector<parser::ParseError> warnings;

  Trace traceParse("Parse");
  auto maybeResult = parser::XMLParser::ParseSVG(fileData, quiet ? nullptr : &warnings);
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

  if (auto path1 = document.svgElement().querySelector("#path1")) {
    std::cout << "Found path1\n";
    path1->setStyle("fill: red");
    path1->setStyle("stroke: white");
  }

  document.setCanvasSize(800, 600);

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
  RendererSkia renderer;
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
