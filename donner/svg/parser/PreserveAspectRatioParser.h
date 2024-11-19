#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::parser {

/**
 * Parser for SVG preserveAspectRatio attribute.
 *
 * @see https://www.w3.org/TR/SVG/coords.html#PreserveAspectRatioAttribute
 */
class PreserveAspectRatioParser {
public:
  /**
   * Parse an SVG preserveAspectRatio attribute.
   *
   * @see https://www.w3.org/TR/SVG/coords.html#PreserveAspectRatioAttribute
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
   * @param str Input string, such as "xMidYMid meet".
   * @return Parsed PreserveAspectRatio.
   */
  static ParseResult<PreserveAspectRatio> Parse(std::string_view str);
};

}  // namespace donner::svg::parser
