#pragma once

#include <type_traits>

#include "tiny_skia/wide/backend/BackendConfig.h"

namespace tiny_skia::wide {

enum class SimdBuildMode {
  kNative,
  kScalar,
};

using SimdBackend = backend::SimdBackend;

template <typename T>
[[nodiscard]] constexpr T genericBitBlend(T mask, T y, T n) {
  static_assert(std::is_integral_v<T>, "genericBitBlend requires integral type");
  return n ^ ((n ^ y) & mask);
}

[[nodiscard]] float fasterMin(float lhs, float rhs);
[[nodiscard]] float fasterMax(float lhs, float rhs);
[[nodiscard]] SimdBuildMode configuredSimdBuildMode();
[[nodiscard]] const char* configuredSimdBuildModeName();
[[nodiscard]] SimdBackend configuredSimdBackend();
[[nodiscard]] const char* configuredSimdBackendName();

}  // namespace tiny_skia::wide
