#pragma once
/// @file
/// The error latch every IR program builds through.

#include <optional>
#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/**
 * Latches the first builder error so a program can be transliterated linearly. On error every
 * subsequent expression receives a dummy `0.0f`; the resulting cascade errors are ignored because
 * only the first is reported. The inputs are static, so any latched error is a Donner bug
 * surfaced by the golden test, never a runtime condition.
 */
struct ErrorLatch {
  std::optional<ShaderError> error;  //!< First error, if any.

  /// Unwraps an expression result. @param result Result to unwrap.
  IrExpr operator()(ShaderResult<IrExpr>&& result) {
    if (result.hasError()) {
      if (!error) {
        error = std::move(result).error();
      }
      return LiteralF32(0.0f);
    }
    return std::move(result).result();
  }

  /// Unwraps a type result. @param result Result to unwrap.
  IrType operator()(ShaderResult<IrType>&& result) {
    if (result.hasError()) {
      if (!error) {
        error = std::move(result).error();
      }
      return IrType::F32();
    }
    return std::move(result).result();
  }

  /// Latches a status. @param status Status to check.
  void ok(ShaderStatus&& status) {
    if (status.hasError() && !error) {
      error = std::move(status).error();
    }
  }
};

}  // namespace donner::gpu::shader::programs
