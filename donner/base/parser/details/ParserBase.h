#pragma once
/// @file

#include <span>

#include "donner/base/ParseResult.h"

namespace donner::base::parser {

/**
 * Base class for parsers, containing common functionality such as maintaining the current
 * parse location, skipping whitespace, and parsing numbers.
 */
class ParserBase {
public:
  /**
   * Create a new parser.
   *
   * @param str String to parse.
   */
  explicit ParserBase(std::string_view str);

protected:
  /**
   * Removes and returns @a count characters from the remaining string.
   *
   * @param count Number of characters to return, this must be less than @p remaining_.size()
   * @return std::string_view Returned string.
   */
  std::string_view take(size_t count);

  /**
   * Remove whitespace characters, from the front of @p remaining_ if they exist, where whitespace
   * is based on the result of isWhitespace().
   */
  void skipWhitespace();

  /**
   * Remove whitespace characters and up to one ',' character from the front of @p remaining_ if
   * they exist.
   */
  void skipCommaWhitespace();

  /**
   * Returns if the character is whitespace, where whitespace is defined as being either U+000A LINE
   * FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020 SPACE.
   *
   * @param ch Character to test.
   */
  bool isWhitespace(char ch) const;

  /**
   * Return the location of the parser's cursor, in characters from the start of the screen.
   *
   * @param index Index of the offset relative to the cursor to return, default is 0.
   */
  FileOffset currentOffset(int index = 0) const;

  /**
   * Returns the number of characters consumed by the parser.
   */
  size_t consumedChars() const;

  /**
   * Read a number, note that this does not skip whitespace.
   */
  ParseResult<double> readNumber();

  /**
   * Read @p resultStorage.size() numbers, separated by whitespace and an optional comma.
   *
   * @param resultStorage Location where the numbers will be stored, the number of parameters is
   *   based on the size of the span.
   */
  std::optional<ParseError> readNumbers(std::span<double> resultStorage);

  /// The original string.
  const std::string_view str_;  // NOLINT: Protected visibility for inheriting parsers to use.

  /// A slice of the remaining characters to parse within \ref str_.
  std::string_view remaining_;  // NOLINT: Protected visibility for inheriting parsers to use.
};

}  // namespace donner::base::parser
