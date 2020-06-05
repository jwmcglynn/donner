#pragma once

#include <string_view>

#include "src/base/transform.h"
#include "src/svg/parser/parse_result.h"

namespace donner {

class TransformParser {
public:
  // Parse an SVG transform attribute.
  // See https://www.w3.org/TR/css-transforms-1/#svg-transform
  //
  // @param str String corresponding to the SVG transform attribute.
  static ParseResult<Transformd> Parse(std::string_view str);
};

}  // namespace donner
