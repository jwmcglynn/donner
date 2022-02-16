#include <span>

#include "src/base/parser/number_parser.h"

namespace donner {

class ParserBase {
public:
  ParserBase(std::string_view str);

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
   */
  int currentOffset();

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

  const std::string_view str_;
  std::string_view remaining_;
};

}  // namespace donner
