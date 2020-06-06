#pragma once

#include <string_view>

#include "src/svg/core/path_spline.h"
#include "src/svg/parser/parse_result.h"

namespace donner {

class PathParser {
public:
  /**
   * Parse an SVG path "d"-string.
   * See https://www.w3.org/TR/SVG/paths.html#PathData
   *
   * Note that this parser may return both an error and a partial path, since path parsing will
   * return anything that it has parsed before it encountered the error. The caller should use both
   * ParseResult::hasResult() and ParseResult::hasError() to determine what has been returned.
   *
   * @param d String corresponding to the SVG <path d="..."> parameter.
   * @return Parsed PathSpline and/or an error.
   */
  static ParseResult<PathSpline> Parse(std::string_view d);
};

}  // namespace donner
