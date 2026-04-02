#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tiny_skia {

using LengthU32 = std::uint32_t;

constexpr LengthU32 kLengthU32One = 1;

/// Equal to `1.0 / (1 << 12)`.
constexpr float kScalarNearlyZero = 1.0f / 4096.0f;

template <typename T>
constexpr T bound(T min, T value, T max) {
  return std::min(max, std::max(min, value));
}

[[nodiscard]] inline bool isNearlyZeroWithinTolerance(float value, float tolerance) {
  return std::abs(value) <= tolerance;
}

[[nodiscard]] inline bool isNearlyZero(float value) {
  return isNearlyZeroWithinTolerance(value, kScalarNearlyZero);
}

[[nodiscard]] inline bool isNearlyEqual(float a, float b) {
  return std::abs(a - b) <= kScalarNearlyZero;
}

[[nodiscard]] inline bool isNearlyEqualWithinTolerance(float a, float b, float tolerance) {
  return std::abs(a - b) <= tolerance;
}

/// Returns `1.0 / value`.
[[nodiscard]] inline float invert(float value) { return 1.0f / value; }

int leftShift(int32_t value, int32_t shift);
long long leftShift64(long long value, int32_t shift);
float approxPowf(float x, float y);

}  // namespace tiny_skia
