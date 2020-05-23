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

  return ExplainMatchResult(message, arg.error().reason, result_listener);
}

}  // namespace donner
