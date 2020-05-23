#pragma once

#include <optional>
#include <string>

#include "src/base/utils.h"
#include "src/svg/parser/parse_error.h"

namespace donner {

/**
 * A parser result, which may contain a result of type T, or an error, or both.
 *
 * @tparam T Result type.
 */
template <typename T>
class ParseResult {
public:
  // Construct from a successful result.
  ParseResult(T&& result) : result_(std::move(result)) {}

  // Construct from an error.
  ParseResult(ParseError&& error) : error_(std::move(error)) {}

  // Return a result, but also an error. Used in the case where partial parse results may be
  // returned.
  ParseResult(T&& result, ParseError&& error)
      : result_(std::move(result)), error_(std::move(error)) {}

  T& result() & {
    UTILS_RELEASE_ASSERT(hasResult());
    return result_.value();
  }

  T&& result() && {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::move(result_.value());
  }

  const T& result() const& {
    UTILS_RELEASE_ASSERT(hasResult());
    return result_.value();
  }

  ParseError& error() & {
    UTILS_RELEASE_ASSERT(hasError());
    return error_.value();
  }

  ParseError&& error() && {
    UTILS_RELEASE_ASSERT(hasError());
    return std::move(error_.value());
  }

  const ParseError& error() const& {
    UTILS_RELEASE_ASSERT(hasError());
    return error_.value();
  }

  bool hasResult() const { return result_.has_value(); }
  bool hasError() const { return error_.has_value(); }

private:
  std::optional<T> result_;
  std::optional<ParseError> error_;
};

}  // namespace donner
