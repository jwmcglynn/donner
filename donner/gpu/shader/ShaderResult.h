#pragma once
/// @file
/// Result type returned by fallible \c donner::gpu::shader APIs.

#include <ostream>
#include <type_traits>
#include <variant>

#include "donner/base/Utils.h"
#include "donner/gpu/shader/ShaderError.h"

namespace donner::gpu::shader {

/**
 * Result of a fallible shader IR operation: exactly one of a value of type \a T or a
 * \ref ShaderError. Mirrors the accessor shape of \c donner::gpu::Result so call sites read the
 * same across the GPU module, but carries the IR-local error type.
 *
 * @tparam T Result type.
 */
template <typename T>
class ShaderResult {
public:
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, ShaderError>,
                "ShaderResult<ShaderError> is ambiguous; use ShaderStatus");

  /**
   * Construct from a successful result.
   *
   * @param result Result value.
   */
  /* implicit */ ShaderResult(T&& result) : value_(std::move(result)) {}

  /**
   * Construct from a successful result by copy.
   *
   * @param result Result value.
   */
  /* implicit */ ShaderResult(const T& result) : value_(result) {}

  /**
   * Construct from an error.
   *
   * @param error Error value.
   */
  /* implicit */ ShaderResult(ShaderError&& error) : value_(std::move(error)) {}

  /**
   * Construct from an error by copy.
   *
   * @param error Error value.
   */
  /* implicit */ ShaderResult(const ShaderError& error) : value_(error) {}

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
  ShaderError& error() & {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<ShaderError>(value_);
  }

  /**
   * Returns the contained error (move).
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  ShaderError&& error() && {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<ShaderError>(std::move(value_));
  }

  /**
   * Returns the contained error.
   *
   * @pre There is a valid error, i.e. \ref hasError() returns true.
   */
  const ShaderError& error() const& {
    UTILS_RELEASE_ASSERT(hasError());
    return std::get<ShaderError>(value_);
  }

  /// Returns true if this result contains a value.
  bool hasResult() const noexcept { return std::holds_alternative<T>(value_); }

  /// Returns true if this result contains an error.
  bool hasError() const noexcept { return std::holds_alternative<ShaderError>(value_); }

private:
  std::variant<T, ShaderError> value_;
};

/**
 * Status of a fallible shader IR operation with no result value. Construct success values with
 * \ref OkShaderStatus().
 */
using ShaderStatus = ShaderResult<std::monostate>;

/// Returns a successful \ref ShaderStatus.
inline ShaderStatus OkShaderStatus() {
  return ShaderStatus(std::monostate());
}

/**
 * gtest PrintTo support for readable failure messages.
 *
 * @param result Result to print.
 * @param os Output stream.
 */
template <typename T>
void PrintTo(const ShaderResult<T>& result, std::ostream* os) {
  if (result.hasError()) {
    *os << "ShaderResult { error: " << result.error() << " }";
  } else if constexpr (std::is_same_v<T, std::monostate>) {
    *os << "ShaderStatus { ok }";
  } else if constexpr (requires(std::ostream& s, const T& v) { s << v; }) {
    *os << "ShaderResult { " << result.result() << " }";
  } else {
    *os << "ShaderResult { <value> }";
  }
}

}  // namespace donner::gpu::shader
