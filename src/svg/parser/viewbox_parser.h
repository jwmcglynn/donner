#pragma once
/// @file

#include <string_view>

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"

namespace donner::svg::parser {

/**
 * Parse an SVG viewBox attribute, such as `0 0 100 100`.
 *
 * @see https://www.w3.org/TR/SVG/coords.html#ViewBoxAttribute
 */
class ViewboxParser {
public:
  /**
   * Parse an SVG viewBox attribute, such as `0 0 100 100`.
   *
   * @see https://www.w3.org/TR/SVG/coords.html#ViewBoxAttribute
   *
   * It parses a string containing the following values:
   * ```
   * <min-x>,? <min-y>,? <width>,? <height>
   * ```
   *
   * Each parameter is a `<number>` type, as parsed by \ref NumberParser. `<width>` and `<height>`
   * must be positive, but the caller must ensure that they are non-zero; a value of zero should
   * disable rendering of the element.
   *
   * @param str Input string.
   * @return Parsed box, or an error.
   */
  static ParseResult<Boxd> Parse(std::string_view str);
};

}  // namespace donner::svg::parser
