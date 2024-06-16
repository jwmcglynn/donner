#pragma once
/// @file

#include <string_view>

#include "src/base/parser/parse_result.h"

namespace donner::base::parser {

/**
 * Parser for floating point numbers in SVG or CSS, corresponding to the CSS3 `<number-token>`
 * definition from https://www.w3.org/TR/css-syntax-3/#number-token-diagram.
 */
class NumberParser {
public:
  /**
   * Container for parse result, containing the parsed result and the number of characters that were
   * consumed to parse it.
   */
  struct Result {
    double number = 0.0;       ///< The parsed result.
    size_t consumedChars = 0;  ///< The number of characters of the input string that were consumed
                               ///< to parse the number.
  };

  /**
   * Options to modify the parsing behavior.
   */
  struct Options {
    /**
     * If this is false and the number is out of range, returns an infinite value instead of
     * returning an error.  With this set to false, assuming the input string starts with a number,
     * all parses should return successful ParseResults.
     */
    bool forbidOutOfRange = true;

    /// Construct a default options struct.
    Options() {}
  };

  /**
   * Parse an SVG number.
   *
   * This corresponds from the CSS3 `<number-token>` production:
   * https://www.w3.org/TR/css-syntax-3/#number-token-diagram
   *
   * It contains:
   * - Optionally a sign, '-' or '+'
   * - Zero or more decimal digits followed by a dot '.', followed by zero or more decimal digits
   * - Optionally, an exponent composed of 'e' or 'E' followed by an integer.
   *
   * @param str String to parse, not all characters may be consumed.
   * @param options Parser options.
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view str, Options options = Options());
};

}  // namespace donner::base::parser
