#pragma once

#include <gmock/gmock.h>

#include <ostream>

#include "donner/base/ParseResult.h"

namespace donner {

namespace details {

/// Check if a type behaves like \c std::optional, with \c has_value() and \c value() methods.
template <typename T>
concept IsOptionalLike = requires(T t) {
  { t.has_value() } -> std::same_as<bool>;
  t.value();
};

}  // namespace details

/// Outputs a ParseResult to a stream for debugging purposes.
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
 * Given a ParseResult, matches if it contains an error whose reason
 * matches the given errorMessageMatcher (string or matcher).
 *
 * Outputs a readable message on mismatch, e.g.:
 *   Expected: parse error is (starts with "Unexpected")
 *   Actual: parse error reason is "Failed to parse number: not finite"
 *
 *
 * Usage:
 * @code
 * EXPECT_THAT(error, ParseErrorIs("My message"));
 * EXPECT_THAT(error, ParseErrorIs(StartsWith("Err")));
 * @endcode
 *
 * @param errorMessageMatcher Matcher to match the error message against, either a string or gmock
 * matcher.
 */
MATCHER_P(ParseErrorIs, errorMessageMatcher,
          std::string("parse error is (") + testing::PrintToString(errorMessageMatcher) + ")") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  // Extract the actual error reason from arg, if present.
  std::string actualReason;
  bool hasError = false;

  // Case 1: arg is already a ParseError.
  if constexpr (std::is_same_v<ArgType, ParseError>) {
    actualReason = arg.reason;
    hasError = true;
  }
  // Case 2: arg is an optional-like container of ParseError.
  else if constexpr (details::IsOptionalLike<ArgType>) {
    if (arg.has_value()) {
      actualReason = arg.value().reason;
      hasError = true;
    }
  }
  // Case 3: arg is a custom "ParseResult" or similar, with .hasError() / .error().
  else {
    if (arg.hasError()) {
      actualReason = arg.error().reason;
      hasError = true;
    }
  }

  // If we found no error, fail immediately.
  if (!hasError) {
    *result_listener << "which has no error at all";
    return false;
  }

  return testing::ExplainMatchResult(errorMessageMatcher, actualReason, result_listener);
}

/**
 * Given a ParseResult, matches if it contains an error at the given offset.
 *
 * @param lineMatcher Line number of the error.
 * @param offsetMatcher Column offset of the error.
 */
MATCHER_P2(ParseErrorPos, lineMatcher, offsetMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const auto matchFileOffset = [&](const FileOffset& location) {
    if (location.lineInfo.has_value()) {
      return testing::ExplainMatchResult(lineMatcher, location.lineInfo->line, result_listener) &&
             testing::ExplainMatchResult(offsetMatcher, location.lineInfo->offsetOnLine,
                                         result_listener);
    } else {
      if (!testing::ExplainMatchResult(lineMatcher, 0, result_listener)) {
        *result_listener
            << "Line 0 should only be used if the result doesn't contain line number information";
        return false;
      }

      if (!location.offset) {
        *result_listener << "Expected an offset, but the error doesn't contain one";
        return false;
      }

      return testing::ExplainMatchResult(offsetMatcher, location.offset.value(), result_listener);
    }
  };

  if constexpr (std::is_same_v<ArgType, ParseError>) {
    return matchFileOffset(arg.location);
  } else if constexpr (std::is_same_v<ArgType, FileOffset>) {
    return matchFileOffset(arg);
  } else {
    // ParseResult<>
    if (!arg.hasError()) {
      return false;
    }

    return matchFileOffset(arg.error().location);
  }
}

/**
 * Matches if a ParseResult contains an error at the end of the string.
 */
MATCHER(ParseErrorEndOfString, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, ParseError>) {
    return arg.offset == std::nullopt;
  } else {
    if (!arg.hasError()) {
      return false;
    }

    return arg.error().location.offset == std::nullopt;
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
