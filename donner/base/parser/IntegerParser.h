#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"

namespace donner::parser {

/**
 * Parser for positive integers, either decimal or hexadecimal.
 */
class IntegerParser {
public:
  /**
   * Container for parse result, containing the parsed result and the number of characters that were
   * consumed to parse it.
   */
  struct Result {
    uint32_t number = 0;       ///< The parsed result.
    size_t consumedChars = 0;  ///< The number of characters of the input string that were consumed
                               ///< to parse the number.
  };

  /**
   * Parse a positive integer in decimal format.
   *
   * @param str String to parse, not all characters may be consumed.
   * @param options Parser options.
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> Parse(std::string_view str);

  /**
   * Parse a positive integer in hexadecimal format, e.g. 'FFEE'. The input string should not start
   * with a prefix (no '0x').
   *
   * @param str String to parse, not all characters may be consumed.
   * @param options Parser options.
   * @return Result containing the number and the number of characters that were parsed.
   */
  static ParseResult<Result> ParseHex(std::string_view str);
};

}  // namespace donner::parser
