#pragma once

#include <string_view>

#include "src/svg/parser/parse_result.h"

namespace donner {

class NumberParser {
public:
  struct Result {
    double number;
    size_t consumed_chars;
  };

  /**
   * Parse an SVG number.
   *
   * This corresponds from the CSS3 <number-token> production:
   * https: *www.w3.org/TR/css-syntax-3/#number-token-diagram
   *
   * It contains:
   *  * Optionally a sign, '-' or '+'
   *  * Zero or more decimal digits followed by a dot '.', followed by zero or more
   *    decimal digits
   *  * Optionally, an exponent composed of 'e' or 'E' followed by an integer.
   *
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view d);
};

}  // namespace donner
