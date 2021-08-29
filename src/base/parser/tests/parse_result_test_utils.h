#pragma once

#include <gmock/gmock.h>

#include <concepts>
#include <ostream>
#include <string>

#include "src/base/parser/parse_result.h"

namespace donner {

template <typename T>
concept IsOptionalLike = requires(T t) {
  // clang-format off
  { t.has_value() } -> std::same_as<bool>;
  t.value();
  // clang-format on
};

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
 * @param errorMessageMatcher Message to match with, either a string or a gmock matcher.
 */
MATCHER_P(ParseErrorIs, errorMessageMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, ParseError>) {
    return testing::ExplainMatchResult(errorMessageMatcher, arg.reason, result_listener);
  } else if constexpr (IsOptionalLike<ArgType>) {
    if (!arg.has_value()) {
      return false;
    }

    return testing::ExplainMatchResult(errorMessageMatcher, arg.value().reason, result_listener);
  } else {
    if (!arg.hasError()) {
      return false;
    }

    return testing::ExplainMatchResult(errorMessageMatcher, arg.error().reason, result_listener);
  }
}

/**
 * Given a ParseResult, matches if it contains an error at the given offset.
 *
 * @param line Line number of the error.
 * @param offset Column offset of the error.
 */
MATCHER_P2(ParseErrorPos, line, offset, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, ParseError>) {
    return testing::ExplainMatchResult(line, arg.line, result_listener) &&
           testing::ExplainMatchResult(offset, arg.offset, result_listener);
  } else {
    if (!arg.hasError()) {
      return false;
    }

    return testing::ExplainMatchResult(line, arg.error().line, result_listener) &&
           testing::ExplainMatchResult(offset, arg.error().offset, result_listener);
  }
}

/**
 * Matches if a ParseResult contains a result that matches the given value, and that it does not
 * contain an error.
 *
 * @param resultMatcher Value to match with.
 */
MATCHER_P(ParseResultIs, resultMatcher, "") {
  if (!arg.hasResult() || arg.hasError()) {
    return false;
  }

  return testing::ExplainMatchResult(resultMatcher, arg.result(), result_listener);
}

/**
 * Matches if a ParseResult contains a result that matches the given value, and that it does not
 * contain an error.
 *
 * @param resultMatcher Result to match with.
 * @param errorMessageMatcher Parse error message to match with, either a string or a gmock matcher.
 */
MATCHER_P2(ParseResultAndError, resultMatcher, errorMessageMatcher, "") {
  if (!arg.hasResult() || !arg.hasError()) {
    return false;
  }

  return testing::ExplainMatchResult(resultMatcher, arg.result(), result_listener) &&
         testing::ExplainMatchResult(errorMessageMatcher, arg.error(), result_listener);
}

}  // namespace donner
