#pragma once
/// @file
/// Error type returned by the \c donner::gpu::shader IR builder and layout engine.

#include <ostream>
#include <string>

#include "donner/base/RcString.h"

namespace donner::gpu::shader {

/**
 * An error from IR construction, validation, or layout computation.
 *
 * The IR builder is fail-closed: ill-typed construction returns a ShaderError, never asserts on
 * input. Messages name the offending operand or rule; \ref nodeLabel carries a caller-supplied
 * source-location-like label (or the node kind when the caller did not supply one) so failures
 * localize without a rerun.
 *
 * This type is intentionally local to the shader IR rather than reusing \c donner::gpu::GpuError:
 * the runtime's error taxonomy describes device/resource failures (handles, descriptors, limits),
 * not type-checking diagnostics, and the IR does not depend on runtime headers.
 */
struct ShaderError {
  std::string message;  //!< Human-readable failure reason.
  RcString nodeLabel;   //!< Label of the node or rule that failed.

  /**
   * Equality operator.
   *
   * @param other Error to compare against.
   */
  bool operator==(const ShaderError& other) const = default;

  /**
   * Ostream output operator, outputs `<label>: <message>`.
   *
   * @param os Output stream.
   * @param error Error to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const ShaderError& error) {
    return os << error.nodeLabel << ": " << error.message;
  }
};

/**
 * gtest PrintTo support for readable failure messages.
 *
 * @param error Error to print.
 * @param os Output stream.
 */
inline void PrintTo(const ShaderError& error, std::ostream* os) {
  *os << error;
}

}  // namespace donner::gpu::shader
