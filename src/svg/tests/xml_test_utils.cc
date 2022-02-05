#include "src/svg/tests/xml_test_utils.h"

#include "src/svg/xml/xml_parser.h"

namespace donner {

namespace {

static constexpr std::string_view kPrefix(
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">\n  ");

static constexpr std::string_view kSuffix("\n</svg>");

}  // namespace

SVGDocument instantiateSubtree(std::string_view str) {
  std::vector<char> fileData;
  fileData.reserve(kPrefix.size() + str.size() + kSuffix.size() + 1);
  fileData.insert(fileData.end(), kPrefix.begin(), kPrefix.end());
  fileData.insert(fileData.end(), str.begin(), str.end());
  fileData.insert(fileData.end(), kSuffix.begin(), kSuffix.end());
  fileData.push_back('\0');

  std::vector<ParseError> warnings;
  auto maybeResult = XMLParser::ParseSVG(fileData, &warnings);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << std::endl;
    return SVGDocument();
  }

  return std::move(maybeResult.result());
}

ParsedFragment<> instantiateSubtreeElement(std::string_view str) {
  SVGDocument document = instantiateSubtree(str);

  auto maybeElement = document.svgElement().firstChild();
  if (maybeElement) {
    return ParsedFragment<>{std::move(document), std::move(maybeElement.value())};
  } else {
    UTILS_RELEASE_ASSERT(false && "No element found in subtree.");
  }
}

}  // namespace donner
