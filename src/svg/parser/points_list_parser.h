#pragma once

#include <string_view>

#include "src/base/parser/parse_result.h"
#include "src/base/vector2.h"

namespace donner::svg {

/**
 * Parse a SVG "points" attribute, used to specify line paths for `<polyline>` and `<polygon>`
 * elements. See https://www.w3.org/TR/SVG2/shapes.html#DataTypePoints
 */
class PointsListParser {
public:
  /**
   * Parse a SVG "points" attribute, used to specify line paths for `<polyline>` and `<polygon>`
   * elements. See https://www.w3.org/TR/SVG2/shapes.html#DataTypePoints
   *
   * ```
   * <points> = [ <number>+ ]#
   * ```
   *
   * A list of numbers separated by whitespace or commas, for example: "10,20 30,40". `<number>` is
   * the same as the CSS number type: "... an integer, or zero or more decimal digits followed by a
   * dot (.) followed by one or more decimal digits and optionally an exponent composed of "e" or
   * "E" and an integer".
   *
   * In between coordinates, there may be optional whitespace and an optional comma. Due to a quirk
   * in the spec, this also means that the path-style string of "-1-2-3-4" is valid and parses as
   * (-1, -2) (-3, -4).
   *
   * Note that this parser may return both an error and a valid result, since the SVG states:
   *
   * > If an odd number of coordinates is provided, then the element is in error, with the same user
   * > agent behavior as occurs with an incorrectly specified 'path' element. In such error cases
   * > the user agent will drop the last, odd coordinate and otherwise render the shape.
   *
   * If an odd number of coordinates is provided, the parser will return an error and list of valid
   * points. The caller should use both `ParseResult::hasResult()` and `ParseResult::hasError()` to
   * determine what has been returned.
   *
   * @param str a points list, optionally separated by spaces and/or a comma.
   * @return Parsed Points list and/or an error.
   */
  static ParseResult<std::vector<Vector2d>> Parse(std::string_view str);
};

}  // namespace donner::svg
