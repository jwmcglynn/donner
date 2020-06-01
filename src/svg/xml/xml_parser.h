#pragma once

#include <span>

#include "src/svg/parser/parse_result.h"
#include "src/svg/svg_document.h"

namespace donner {

class XMLParser {
public:
  static ParseResult<SVGDocument> parseSVG(std::span<char> str);
};

}  // namespace donner