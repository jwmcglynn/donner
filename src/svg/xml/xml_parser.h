#pragma once

#include <span>

#include "src/svg/parser/parse_result.h"
#include "src/svg/svg_document.h"

namespace donner {

class XMLParser {
public:
  static ParseResult<SVGDocument> ParseSVG(std::span<char> str,
                                           std::vector<ParseError>* out_warnings);
};

}  // namespace donner