#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

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
    std::cerr << "USAGE: xml_tool <filename>" << std::endl;
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
  return 0;
}

}  // namespace donner
