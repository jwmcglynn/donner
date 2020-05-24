#pragma once

#include <gmock/gmock.h>

#include <ostream>
#include <string>

#include "src/svg/parser/parse_result.h"

namespace donner {

template <typename T>
void PrintTo(const ParseResult<T>& result, std::ostream* os) {
  *os << "ParseResult {";
  if (result.hasResult()) {
    *os << " result: " << testing::PrintToString(result.result());
  }
  if (result.hasError()) {
    *os << " error: " << result.error();
  }
  *os << " }";
}

/**
 * Matches if the result does not contain a ParseError.
 */
MATCHER(NoParseError, "") {
  return !arg.hasError();
}

/**
 * Given a ParseResult, matches if it contains an error with the given message.
 *
 * Usage:
 * @code
 * EXPECT_THAT(error, ParseErrorIs("My message"));
 * EXPECT_THAT(error, ParseErrorIs(StartsWith("Err")));
 * @endcode
 *
 * @param message Message to match with, either a string or a gmock matcher.
 */
MATCHER_P(ParseErrorIs, message, "") {
  if (!arg.hasError()) {
    return false;
  }

  return testing::ExplainMatchResult(message, arg.error().reason, result_listener);
}

/**
 * Given a ParseResult, matches if it contains an error at the given offset.
 *
 * @param line Line number of the error.
 * @param offset Column offset of the error.
 */
MATCHER_P2(ParseErrorPos, line, offset, "") {
  if (!arg.hasError()) {
    return false;
  }

  return arg.error().line == line && arg.error().offset == offset;
}

/**
 * Matches if a ParseResult contains a result that matches the given value, and that it does not
 * contain an error.
 *
 * @param value Value to match with.
 */
MATCHER_P(ParseResultIs, value, "") {
  if (!arg.hasResult() || arg.hasError()) {
    return false;
  }

  return testing::ExplainMatchResult(value, arg.result(), result_listener);
}

}  // namespace donner
