#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "src/renderer/renderer_skia.h"
#include "src/svg/svg_element.h"
#include "src/svg/xml/xml_parser.h"

namespace donner {

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << TypeToString(element.type()) << ", id: '" << element.id() << "'";
  if (element.type() == ElementType::SVG) {
    if (auto viewbox = element.cast<SVGSVGElement>().viewbox()) {
      std::cout << ", viewbox: " << *viewbox;
    }
  }
  std::cout << std::endl;
  for (auto elm = element.firstChild(); elm; elm = elm->nextSibling()) {
    DumpTree(elm.value(), depth + 1);
  }
}

extern "C" int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count." << std::endl;
    std::cerr << "USAGE: renderer_tool <filename>" << std::endl;
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << std::endl;
    return 2;
  }

  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);

  std::vector<char> fileData(fileLength + 1);
  file.read(fileData.data(), fileLength);

  std::vector<ParseError> warnings;
  auto maybeResult = XMLParser::ParseSVG(fileData, &warnings);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << std::endl;
    return 3;
  }

  std::cout << "Parsed successfully." << std::endl;

  if (!warnings.empty()) {
    std::cout << "Warnings:" << std::endl;
    for (auto& w : warnings) {
      std::cout << "  " << w.line << ":" << w.offset << ": " << w.reason << std::endl;
    }
  }

  std::cout << "Tree:" << std::endl;

  SVGDocument document = std::move(maybeResult.result());
  DumpTree(document.svgElement(), 0);

  if (auto path1 = document.svgElement().querySelector("#path1")) {
    std::cout << "Found path1" << std::endl;
    path1->setStyle("fill: red");
    path1->setStyle("stroke: white");
  }

  const size_t kWidth = 800;
  const size_t kHeight = 600;

  RendererSkia renderer(kWidth, kHeight);
  renderer.draw(document);

  constexpr const char* kOutputFilename = "output.png";
  if (renderer.save(kOutputFilename)) {
    std::cout << "Saved to file: " << std::filesystem::absolute(kOutputFilename) << std::endl;
    return 0;
  } else {
    std::cerr << "Failed to save to file: " << std::filesystem::absolute(kOutputFilename)
              << std::endl;
    return 1;
  }
}

}  // namespace donner
