#pragma once

#include <string_view>

#include "src/base/parser/parse_result.h"

namespace donner {

class NumberParser {
public:
  struct Result {
    double number;
    size_t consumed_chars;
  };

  struct Options {
    /**
     * If this is false and the number is out of range, returns an infinite value instead of
     * returning an error.  With this set to false, assuming the input string starts with a number,
     * all parses should return successful ParseResults.
     */
    bool forbid_out_of_range = true;

    Options() {}
  };

  /**
   * Parse an SVG number.
   *
   * This corresponds from the CSS3 <number-token> production:
   * https://www.w3.org/TR/css-syntax-3/#number-token-diagram
   *
   * It contains:
   *  * Optionally a sign, '-' or '+'
   *  * Zero or more decimal digits followed by a dot '.', followed by zero or more
   *    decimal digits
   *  * Optionally, an exponent composed of 'e' or 'E' followed by an integer.
   *
   * @param str String to parse, not all characters may be consumed.
   * @param options Parser options.
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view str, Options options = Options());
};

}  // namespace donner
