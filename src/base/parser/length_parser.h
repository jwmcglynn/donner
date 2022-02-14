#pragma once

#include <string_view>

#include "src/base/length.h"
#include "src/base/parser/parse_result.h"

namespace donner {

class LengthParser {
public:
  struct Result {
    Lengthd length;
    size_t consumedChars;
  };

  struct Options {
    /**
     * If set, the unit specifier is optional, enabling non-zero numbers to be parsed without a
     * suffix, such as "100".
     *
     * When true, this is equivalent to parsing a [ <length-percentage> | <number> ]. If the unit
     * specifier is not found, the Length returned will have Unit::None.
     *
     * This should be true when parsing presentation attributes, see
     * https://www.w3.org/TR/SVG/types.html#syntax.
     */
    bool unitOptional = false;

    Options() {}
  };

  /**
   * Parse a CSS <length-percentage>
   *
   * See https://www.w3.org/TR/css-values/#typedef-length-percentage
   *
   * Equivalent to [ <length> | <percentage> ], where <percentage> will resolve to <length>.
   *
   *  <length> maps to <dimension-token>:
   *    https://www.w3.org/TR/css-syntax-3/#dimension-token-diagram
   *  <percentage> maps to <percentage-token>:
   *    https://www.w3.org/TR/css-syntax-3/#percentage-token-diagram
   *
   *  <dimension-token> = <number-token> <ident-token>
   *  <percentage-token> = <number-token> %
   *  <number-token> = The result of @ref NumberParser, a real number in either fixed or scientific
   *    notation, with optional '+' or '-' prefix.
   *  <ident-token> = -?-? [ a-z A-Z _ or non-ASCII ] [ a-z A-Z _ - or non-ASCII ]
   *    However, LengthParser is limited to valid suffixes for length, as defined by Length::Unit.
   *
   * If the number is 0, the <ident-token> may be omitted since 0 is unitless. This can be extended
   * to all numbers by setting Options::unitOptional to true.
   *
   * Note that this may not consume all input, the caller should handle the result of
   * Result::consumedChars.
   *
   * @param str String to parse, not all characters may be consumed.
   * @param options Parser options.
   * @return Result containing the Length and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view str, Options options = Options());

  /**
   * Parse a unit suffix from a string, such as "px" or "em".
   *
   * @param str String containing the unit suffix, which must be a complete case-insensitive match
   *            for a supported <dimension-token> suffix, or '%' for <percentage-token>.
   * @return Lengthd::Unit corresponding to the suffix, or std::nullopt if there was no match.
   */
  static std::optional<Lengthd::Unit> ParseUnit(std::string_view str);
};

}  // namespace donner
