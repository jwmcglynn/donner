#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererSkia.h"

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
  if (argc != 2) {
    std::cerr << "Unexpected arg count."
              << "\n";
    std::cerr << "USAGE: renderer_tool <filename>"
              << "\n";
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 2;
  }

  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);

  std::vector<char> fileData(fileLength + 1);
  file.read(fileData.data(), static_cast<std::streamsize>(fileLength));

  std::vector<parser::ParseError> warnings;

  Trace traceParse("Parse");
  auto maybeResult = parser::XMLParser::ParseSVG(fileData, &warnings);
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

  std::cout << "Tree:\n";
  DumpTree(document.svgElement(), 0);

  if (auto path1 = document.svgElement().querySelector("#path1")) {
    std::cout << "Found path1\n";
    path1->setStyle("fill: red");
    path1->setStyle("stroke: white");
  }

  document.setCanvasSize(800, 600);

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
