#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"

namespace donner::svg::parser {

/**
 * Parser for floating point numbers in SVG or CSS, corresponding to the CSS3 `<number-token>`
 * definition from https://www.w3.org/TR/css-syntax-3/#number-token-diagram.
 */
class Number2dParser {
public:
  /**
   * Container for parse result, containing the parsed result and the number of characters that were
   * consumed to parse it.
   */
  struct Result {
    double numberX = 0.0;      ///< The first parsed number.
    double numberY = 0.0;      ///< The second parsed number.
    size_t consumedChars = 0;  ///< The number of characters of the input string that were consumed
                               ///< to parse the number.
  };

  /**
   * Parse an SVG filter `<number-optional-number>` value, which is used to specify either a single
   * number or two numbers representing an X/Y pair for a filter operation.
   *
   * @see https://www.w3.org/TR/filter-effects/#typedef-number-optional-number
   *
   * Each number is a `<number-token>`, which may either be an integer, floating point, or
   * scientific notation.
   * - `<number-optional-number> = <number> <number>?`
   *
   * @param str String to parse, not all characters may be consumed.
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view str);
};

}  // namespace donner::svg::parser
