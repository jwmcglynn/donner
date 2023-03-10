#pragma once
/// @file

#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/svg/core/preserve_aspect_ratio.h"

namespace donner::svg {

class PreserveAspectRatioParser {
public:
  /**
   * Parse an SVG preserveAspectRatio attribute.
   *
   * See https://www.w3.org/TR/SVG/coords.html#PreserveAspectRatioAttribute
   *
   * It parses a string containing the following values:
   * ```
   * <align> <meetOrSlice>?
   *
   * <align> =
   *   none
   *   | xMinYMin | xMidYMin | xMaxYMin
   *   | xMinYMid | xMidYMid | xMaxYMid
   *   | xMinYMax | xMidYMax | xMaxYMax
   * <meetOrSlice> = meet | slice
   * ```
   *
   * @param str Input string.
   * @return Parsed PreserveAspectRatio.
   */
  static ParseResult<PreserveAspectRatio> Parse(std::string_view str);
};

}  // namespace donner::svg
