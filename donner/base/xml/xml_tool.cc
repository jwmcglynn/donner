#include <fstream>
#include <iostream>
#include <vector>

#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

using donner::base::parser::ParseError;

void DumpTree(const XMLNode& element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ": " << element.tagName();
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

  std::vector<char> fileData;
  fileData.resize(fileLength);
  file.read(fileData.data(), fileLength);

  auto maybeResult = XMLParser::Parse(std::string_view(fileData.data(), fileData.size()));
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  std::cout << "Tree:\n";
  DumpTree(maybeResult.result().root(), 0);
  return 0;
}

}  // namespace donner::xml