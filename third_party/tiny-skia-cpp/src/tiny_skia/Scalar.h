#pragma once

#include "tiny_skia/FloatingPoint.h"

namespace tiny_skia {

// --- Scalar trait utility functions (matches Rust Scalar trait on f32) ---

/// Returns x * 0.5.
[[nodiscard]] inline constexpr float scalarHalf(float x) { return x * 0.5f; }

/// Returns the average of two values: (a + b) * 0.5.
[[nodiscard]] inline constexpr float scalarAve(float a, float b) { return (a + b) * 0.5f; }

/// Returns x * x.
[[nodiscard]] inline constexpr float scalarSqr(float x) { return x * x; }

/// Returns 1.0 / x.
[[nodiscard]] inline constexpr float scalarInvert(float x) { return 1.0f / x; }

/// Non-panicking clamp: clamps x into [lo, hi].
[[nodiscard]] inline constexpr float scalarBound(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace tiny_skia
