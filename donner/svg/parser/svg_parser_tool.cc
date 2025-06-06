#include <fstream>
#include <iostream>
#include <vector>

#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ", id: '" << element.id() << "'";
  if (element.type() == ElementType::SVG) {
    if (auto viewBox = element.cast<SVGSVGElement>().viewBox()) {
      std::cout << ", viewBox: " << *viewBox;
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
    std::cerr << "USAGE: svg_parser_tool <filename>\n";
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

  std::string fileData;
  fileData.resize(fileLength);
  file.read(fileData.data(), fileLength);

  std::vector<ParseError> warnings;
  auto maybeResult = parser::SVGParser::ParseSVG(fileData, &warnings);
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
