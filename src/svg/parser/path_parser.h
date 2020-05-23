#pragma once

#include <string_view>

#include "src/svg/core/path_spline.h"
#include "src/svg/parser/parse_result.h"

namespace donner {

class PathParser {
public:
  // Parse an SVG path "d"-string.
  //
  // @param d String corresponding to the SVG <path d="..."> parameter.
  static ParseResult<PathSpline> parse(std::string_view d);
};

}  // namespace donner
