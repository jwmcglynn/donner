#pragma once
/// @file

#include <optional>

#include "src/base/parser/parse_error.h"
#include "src/base/utils.h"

namespace donner::base::parser {

/**
 * A parser result, which may contain a result of type \a T, or an error, or both.
 *
 * @tparam T Result type.
 */
template <typename T>
class ParseResult {
public:
  /**
   * Construct from a successful result.
   */
  /* implicit */ ParseResult(T&& result) : result_(std::move(result)) {}

  /**
   * Construct from a successful result.
   */
  /* implicit */ ParseResult(const T& result) : result_(result) {}

  /**
   * Construct from an error.
   */
  /* implicit */ ParseResult(ParseError&& error) : error_(std::move(error)) {}

  /**
   * Return a result, but also an error. Used in the case where partial parse results may be
   * returned.
   */
  ParseResult(T&& result, ParseError&& error)
      : result_(std::move(result)), error_(std::move(error)) {}

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T& result() & {
    UTILS_RELEASE_ASSERT(hasResult());
    return result_.value();
  }

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T&& result() && {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::move(result_.value());
  }

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  const T& result() const& {
    UTILS_RELEASE_ASSERT(hasResult());
    return result_.value();
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  ParseError& error() & {
    UTILS_RELEASE_ASSERT(hasError());
    return error_.value();
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  ParseError&& error() && {
    UTILS_RELEASE_ASSERT(hasError());
    return std::move(error_.value());
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  const ParseError& error() const& {
    UTILS_RELEASE_ASSERT(hasError());
    return error_.value();
  }

  /// Returns true if this ParseResult contains a valid result.
  bool hasResult() const noexcept { return result_.has_value(); }

  /// Returns true if this ParseResult contains an error.
  bool hasError() const noexcept { return error_.has_value(); }

  /**
   * Map the result of this ParseResult to a new type, by transforming the result with the provided
   * functor.
   *
   * @tparam Target The type to map to.
   * @tparam Functor The functor type.
   * @param functor The functor to apply to the result.
   */
  template <typename Target, typename Functor>
  ParseResult<Target> map(const Functor& functor) && {
    if (hasResult()) {
      return ParseResult<Target>(functor(std::move(result_.value())));
    } else {
      return ParseResult<Target>(std::move(error_.value()));
    }
  }

  /**
   * Map the error of this ParseResult to a new type, by transforming the error with the provided
   * functor.
   *
   * @tparam Target The type to map to.
   * @tparam Functor The functor type.
   * @param functor The functor to apply to the error.
   */
  template <typename Target, typename Functor>
  ParseResult<Target> mapError(const Functor& functor) && {
    if (hasError()) {
      return ParseResult<Target>(functor(std::move(error_.value())));
    } else {
      return ParseResult<Target>(std::move(result_.value()));
    }
  }

private:
  std::optional<T> result_;
  std::optional<ParseError> error_;
};

}  // namespace donner::base::parser

// Re-export in svg and css namespaces for convenience.
namespace donner::svg::parser {
using donner::base::parser::ParseResult;
}  // namespace donner::svg::parser

namespace donner::css::parser {
using donner::base::parser::ParseResult;
}  // namespace donner::css::parser
