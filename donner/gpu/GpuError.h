#pragma once
/// @file
/// Explicit error types returned by \c donner::gpu APIs.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace donner::gpu {

/**
 * Category of a GPU runtime error.
 *
 * Every invalid descriptor, handle, or state transition fails closed by returning one of these
 * categories wrapped in a \ref GpuError, in release builds too. Asserts are reserved for
 * programmer invariants inside the runtime; invalid input never aborts.
 */
enum class GpuErrorType : uint8_t {
  InvalidDescriptor,  //!< A descriptor field is malformed (zero size, bad range, misalignment).
  InvalidHandle,      //!< A handle is null, destroyed, or has a stale generation.
  DeviceMismatch,     //!< A handle created by one device was used on another device.
  UsageMismatch,      //!< A resource lacks the usage flag required by the operation.
  OutOfBounds,        //!< An offset, size, or copy region exceeds the resource bounds.
  LimitExceeded,      //!< A count or dimension exceeds a documented device limit.
  InvalidState,       //!< An operation was issued in an invalid state (encoder state machine).
  Unsupported,        //!< The requested feature is not supported by this runtime.
};

/**
 * Ostream output operator for \ref GpuErrorType, e.g. `InvalidDescriptor`.
 *
 * @param os Output stream.
 * @param value Error type to output.
 */
std::ostream& operator<<(std::ostream& os, GpuErrorType value);

/**
 * An explicit error result from a \c donner::gpu operation, carrying the error category and a
 * human-readable message that names the offending field or state so a failing test localizes the
 * bug without a rerun.
 */
struct GpuError {
  GpuErrorType type = GpuErrorType::InvalidDescriptor;  //!< Error category.
  std::string message;                                  //!< Human-readable failure reason.

  /**
   * Returns the name of an error type, e.g. `"InvalidDescriptor"`.
   *
   * @param type Error type to format.
   */
  static std::string_view TypeToString(GpuErrorType type);

  /// Formats this error as `<type>: <message>`.
  std::string toString() const;

  /**
   * Equality operator.
   *
   * @param other Error to compare against.
   */
  bool operator==(const GpuError& other) const = default;

  /**
   * Ostream output operator, outputs `<type>: <message>`.
   *
   * @param os Output stream.
   * @param error Error to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const GpuError& error);
};

/**
 * gtest PrintTo support for readable test failure messages.
 *
 * @param error Error to print.
 * @param os Output stream.
 */
inline void PrintTo(const GpuError& error, std::ostream* os) {
  *os << error;
}

}  // namespace donner::gpu
