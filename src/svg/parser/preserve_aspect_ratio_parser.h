#pragma once

#include <string_view>

#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/parser/parse_result.h"

namespace donner {

class PreserveAspectRatioParser {
public:
  /**
   * Parse an SVG preserveAspectRatio attribute.
   *
   * See https://www.w3.org/TR/SVG/coords.html#PreserveAspectRatioAttribute
   *
   * It parses a string containing the following values:
   *  <align> <meetOrSlice>?
   *
   *  <align> =
   *    none
   *    | xMinYMin | xMidYMin | xMaxYMin
   *    | xMinYMid | xMidYMid | xMaxYMid
   *    | xMinYMax | xMidYMax | xMaxYMax
   *  <meetOrSlice> = meet | slice
   *
   *
   * @return Parsed PreserveAspectRatio.
   */
  static ParseResult<PreserveAspectRatio> Parse(std::string_view d);
};

}  // namespace donner
