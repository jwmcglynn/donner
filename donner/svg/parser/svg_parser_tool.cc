#include <iostream>
#include <sstream>

#include "donner/base/FileUtils.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/TerminalEscape.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ", id: '" << EscapeTerminalText(element.id()) << "'";
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

}  // namespace donner::svg

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count.\n";
    std::cerr << "USAGE: svg_parser_tool <filename>\n";
    return 1;
  }

  auto fileResult =
      donner::ReadFileBounded(argv[1], donner::svg::parser::SVGParser::kDefaultMaximumInputSize);
  const auto* fileData = std::get_if<std::string>(&fileResult);
  if (fileData == nullptr) {
    std::cerr << donner::FileReadErrorMessage(std::get<donner::FileReadError>(fileResult)) << ": "
              << donner::EscapeTerminalText(argv[1]) << "\n";
    return 2;
  }

  donner::ParseWarningSink warnings;
  auto maybeResult = donner::svg::parser::SVGParser::ParseSVG(*fileData, warnings);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::ostringstream diagnostic;
    diagnostic << e;
    std::cerr << "Parse Error " << donner::EscapeTerminalText(diagnostic.str()) << "\n";
    return 3;
  }

  std::cout << "Parsed successfully.\n";

  if (warnings.hasWarnings()) {
    std::cout << "Warnings:\n";
    for (auto& w : warnings.warnings()) {
      std::ostringstream diagnostic;
      diagnostic << w;
      std::cout << "  " << donner::EscapeTerminalText(diagnostic.str()) << "\n";
    }
  }

  std::cout << "Tree:\n";
  DumpTree(maybeResult.result().svgElement(), 0);
  return 0;
}
