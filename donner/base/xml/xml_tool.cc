#include <iostream>
#include <sstream>

#include "donner/base/FileUtils.h"
#include "donner/base/TerminalEscape.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

void DumpTree(const XMLNode& element, std::ostream& output, int depth) {
  for (int i = 0; i < depth; ++i) {
    output << "  ";
  }

  std::ostringstream tagName;
  tagName << element.tagName();
  output << element.type() << ": " << EscapeTerminalText(tagName.str()) << '\n';
  for (std::optional<XMLNode> child = element.firstChild(); child.has_value();
       child = child->nextSibling()) {
    DumpTree(*child, output, depth + 1);
  }
}

}  // namespace donner::xml

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: xml_tool <filename>\n";
    return 1;
  }

  const size_t maximumInputSize = donner::xml::XMLParser::Options().maximumInputSize;
  auto fileResult = donner::ReadFileBounded(argv[1], maximumInputSize);
  const auto* fileData = std::get_if<std::string>(&fileResult);
  if (fileData == nullptr) {
    std::cerr << donner::FileReadErrorMessage(std::get<donner::FileReadError>(fileResult)) << ": "
              << donner::EscapeTerminalText(argv[1]) << "\n";
    return 2;
  }

  auto maybeResult = donner::xml::XMLParser::Parse(*fileData);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::ostringstream diagnostic;
    diagnostic << e;
    std::cerr << "Parse Error " << donner::EscapeTerminalText(diagnostic.str()) << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  std::cout << "Tree:\n";
  donner::xml::DumpTree(maybeResult.result().root(), std::cout, 0);
  return 0;
}
