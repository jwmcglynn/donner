#include "donner/svg/tests/XMLTestUtils.h"

#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/xml/XMLParser.h"

namespace donner::svg {

namespace {

static constexpr std::string_view kSuffix("\n</svg>");

}  // namespace

SVGDocument instantiateSubtree(std::string_view str, const parser::XMLParser::Options& options,
                               const Vector2i& size) {
  parser::XMLParser::InputBuffer fileData;
  const std::string prefix = std::string(
                                 "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                                 "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"") +
                             std::to_string(size.x) + "\" height=\"" + std::to_string(size.y) +
                             "\">\n  ";

  fileData.reserve(prefix.size() + str.size() + kSuffix.size() + 1);
  fileData.insert(fileData.end(), prefix.begin(), prefix.end());
  fileData.insert(fileData.end(), str.begin(), str.end());
  fileData.insert(fileData.end(), kSuffix.begin(), kSuffix.end());
  fileData.push_back('\0');

  std::vector<parser::ParseError> warnings;
  auto maybeResult = parser::XMLParser::ParseSVG(fileData, &warnings, options);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e << "\n";
    return SVGDocument();
  }

  // Set the canvas size, this is needed for computed style calculation to succeed.
  auto& document = maybeResult.result();
  document.setCanvasSize(size.x, size.y);

  return std::move(maybeResult.result());
}

ParsedFragment<> instantiateSubtreeElement(std::string_view str,
                                           const parser::XMLParser::Options& options,
                                           Vector2i size) {
  SVGDocument document = instantiateSubtree(str, options, size);

  auto maybeElement = document.svgElement().firstChild();
  if (maybeElement) {
    return ParsedFragment<>{std::move(document), std::move(maybeElement.value())};
  } else {
    UTILS_RELEASE_ASSERT(false && "No element found in subtree.");
  }
}

}  // namespace donner::svg
