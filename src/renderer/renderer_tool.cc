#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "src/renderer/render_context_osmesa.h"
#include "src/renderer/renderer_pathfinder.h"
#include "src/svg/svg_element.h"
#include "src/svg/xml/xml_parser.h"

namespace donner {

std::string_view TypeToString(ElementType type) {
  switch (type) {
    case ElementType::SVG: return "SVG";
    case ElementType::Path: return "Path";
    case ElementType::Rect: return "Rect";
    case ElementType::Unknown: return "Unknown";
  }
}

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
  DumpTree(maybeResult.result().svgElement(), 0);

  const size_t kWidth = 800;
  const size_t kHeight = 600;
  RenderContextOSMesa renderContext(kWidth, kHeight);

  {
    std::string errors;
    if (!renderContext.makeCurrent(&errors)) {
      std::cerr << "RenderContext makeCurrent failure: " << errors << std::endl;
      return 3;
    }
  }

  RendererPathfinder renderer(&RenderContextOSMesa::getProcAddress, kWidth, kHeight);
  renderer.draw(maybeResult.result());
  renderer.render();

  renderContext.savePNG("offscreen.png");
  return 0;
}

}  // namespace donner