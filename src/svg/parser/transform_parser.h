#pragma once

#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/base/transform.h"

namespace donner {

class TransformParser {
public:
  /**
   * Parse an SVG transform attribute.
   * See https://www.w3.org/TR/css-transforms-1/#svg-transform
   *
   * @param str String corresponding to the SVG transform attribute.
   * @return Parsed transform, or an error.
   */
  static ParseResult<Transformd> Parse(std::string_view str);
};

}  // namespace donner
