#pragma once
/// @file

#include <expected>
#include <utility>

#include "donner/base/ParseError.h"
#include "donner/base/Utils.h"

namespace donner {

/**
 * A parser result backed by `std::expected`, containing either a value of type \a T or a
 * \ref ParseError.
 *
 * @tparam T Result type.
 */
template <typename T>
class ParseResult {
public:
  using ExpectedType = std::expected<T, ParseError>;

  /**
   * Construct from a successful result.
   */
  /* implicit */ ParseResult(T&& result) : expected_(std::move(result)) {}

  /**
   * Construct from a successful result.
   */
  /* implicit */ ParseResult(const T& result) : expected_(result) {}

  /**
   * Construct from an error.
   */
  /* implicit */ ParseResult(ParseError&& error) : expected_(std::unexpected(std::move(error))) {}

  /**
   * Construct from an error by value.
   */
  /* implicit */ ParseResult(const ParseError& error) : expected_(std::unexpected(error)) {}

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T& result() & {
    UTILS_RELEASE_ASSERT(hasResult());
    return expected_.value();
  }

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T&& result() && {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::move(expected_.value());
  }

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  const T& result() const& {
    UTILS_RELEASE_ASSERT(hasResult());
    return expected_.value();
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  ParseError& error() & {
    UTILS_RELEASE_ASSERT(hasError());
    return expected_.error();
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  ParseError&& error() && {
    UTILS_RELEASE_ASSERT(hasError());
    return std::move(expected_.error());
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  const ParseError& error() const& {
    UTILS_RELEASE_ASSERT(hasError());
    return expected_.error();
  }

  /// Returns true if this ParseResult contains a valid result.
  bool hasResult() const noexcept { return expected_.has_value(); }

  /// Returns true if this ParseResult contains an error.
  bool hasError() const noexcept { return !expected_.has_value(); }

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
      typename ParseResult<Target>::ExpectedType mapped =
          std::move(expected_).transform(functor);
      return mapped.has_value() ? ParseResult<Target>(std::move(mapped).value())
                                : ParseResult<Target>(std::move(mapped).error());
    } else {
      return ParseResult<Target>(std::move(expected_).error());
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
      typename ParseResult<Target>::ExpectedType mapped =
          std::move(expected_).transform_error(functor);
      return mapped.has_value() ? ParseResult<Target>(std::move(mapped).value())
                                : ParseResult<Target>(std::move(mapped).error());
    } else {
      return ParseResult<Target>(std::move(expected_).value());
    }
  }


  /// Convert this ParseResult to an ExpectedType.
  [[nodiscard]] ExpectedType toExpected() && { return std::move(expected_); }

private:
  ExpectedType expected_;
};

}  // namespace donner
