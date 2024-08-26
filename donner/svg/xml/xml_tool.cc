#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "donner/svg/SVGElement.h"
#include "donner/svg/xml/XMLParser.h"

namespace donner::svg {

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ", id: '" << element.id() << "'";
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
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: xml_tool <filename>\n";
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 2;
  }

  file.seekg(0, std::ios::end);
  const std::streamsize fileLength = file.tellg();
  file.seekg(0);

  parser::XMLParser::InputBuffer fileData;
  fileData.resize(fileLength);
  file.read(fileData.data(), fileLength);

  std::vector<parser::ParseError> warnings;
  auto maybeResult = parser::XMLParser::ParseSVG(fileData, &warnings);
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

  std::cout << "Tree:\n";
  DumpTree(maybeResult.result().svgElement(), 0);
  return 0;
}

}  // namespace donner::svg
