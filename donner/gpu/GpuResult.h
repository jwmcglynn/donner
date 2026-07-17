#pragma once
/// @file
/// Result type returned by fallible \c donner::gpu APIs.

#include <ostream>
#include <type_traits>
#include <variant>

#include "donner/base/Utils.h"
#include "donner/gpu/GpuError.h"

namespace donner::gpu {

/**
 * Result of a fallible GPU operation: exactly one of a value of type \a T or a \ref GpuError.
 *
 * The GPU runtime uses no exceptions; this is the only failure channel. The accessor shape
 * (\ref hasResult / \ref result / \ref error) mirrors \ref donner::ParseResult so call sites read
 * the same across the codebase, but unlike ParseResult a GPU operation never returns a partial
 * result together with an error.
 *
 * @tparam T Result type.
 */
template <typename T>
class Result {
public:
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, GpuError>,
                "Result<GpuError> is ambiguous; use Status for void-returning operations");

  /**
   * Construct from a successful result.
   *
   * @param result Result value.
   */
  /* implicit */ Result(T&& result) : value_(std::move(result)) {}

  /**
   * Construct from a successful result by copy.
   *
   * @param result Result value.
   */
  /* implicit */ Result(const T& result) : value_(result) {}

  /**
   * Construct from an error.
   *
   * @param error Error value.
   */
  /* implicit */ Result(GpuError&& error) : value_(std::move(error)) {}

  /**
   * Construct from an error by copy.
   *
   * @param error Error value.
   */
  /* implicit */ Result(const GpuError& error) : value_(error) {}

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T& result() & {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::get<T>(value_);
  }

  /**
   * Returns the contained result (move).
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  T&& result() && {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::get<T>(std::move(value_));
  }

  /**
   * Returns the contained result.
   *
   * @pre There is a valid result, i.e. \ref hasResult() returns true.
   */
  const T& result() const& {
    UTILS_RELEASE_ASSERT(hasResult());
    return std::get<T>(value_);
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  GpuError& error() & {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<GpuError>(value_);
  }

  /**
   * Returns the contained error (move).
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  GpuError&& error() && {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<GpuError>(std::move(value_));
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  const GpuError& error() const& {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<GpuError>(value_);
  }

  /// Returns true if this Result contains a value.
  bool hasResult() const noexcept { return std::holds_alternative<T>(value_); }

  /// Returns true if this Result contains an error.
  bool hasError() const noexcept { return std::holds_alternative<GpuError>(value_); }

private:
  std::variant<T, GpuError> value_;
};

/**
 * Status of a fallible GPU operation with no result value: either success (a \c std::monostate
 * result) or a \ref GpuError. Construct success values with \ref OkStatus().
 */
using Status = Result<std::monostate>;

/// Returns a successful \ref Status.
inline Status OkStatus() {
  return Status(std::monostate());
}

/**
 * gtest PrintTo support for readable test failure messages.
 *
 * @param result Result to print.
 * @param os Output stream.
 */
template <typename T>
void PrintTo(const Result<T>& result, std::ostream* os) {
  if (result.hasError()) {
    *os << "Result { error: " << result.error() << " }";
  } else if constexpr (std::is_same_v<T, std::monostate>) {
    *os << "Status { ok }";
  } else if constexpr (requires(std::ostream& s, const T& v) { s << v; }) {
    *os << "Result { " << result.result() << " }";
  } else {
    *os << "Result { <value> }";
  }
}

}  // namespace donner::gpu
