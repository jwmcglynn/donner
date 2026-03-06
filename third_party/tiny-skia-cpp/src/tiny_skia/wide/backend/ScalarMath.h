#pragma once

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline float fasterMin(float lhs, float rhs) {
  if (rhs < lhs) {
    return rhs;
  }

  return lhs;
}

[[nodiscard]] inline float fasterMax(float lhs, float rhs) {
  if (lhs < rhs) {
    return rhs;
  }

  return lhs;
}

}  // namespace tiny_skia::wide::backend::scalar
