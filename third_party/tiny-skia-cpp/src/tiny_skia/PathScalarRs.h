#pragma once

#include "tiny_skia/FloatingPoint.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/Point.h"
#include "tiny_skia/Scalar.h"

namespace tiny_skia::pathScalarRs {

// Wrappers for `third_party/tiny-skia/path/src/scalar.rs`.

inline constexpr float SCALAR_NEARLY_ZERO = kScalarNearlyZero;
inline constexpr float SCALAR_ROOT_2_OVER_2 = kScalarRoot2Over2;

inline constexpr float half(float value) { return scalarHalf(value); }
inline constexpr float ave(float a, float b) { return scalarAve(a, b); }
inline constexpr float sqr(float value) { return scalarSqr(value); }
inline constexpr float invert(float value) { return scalarInvert(value); }
inline constexpr float bound(float value, float min, float max) {
  return scalarBound(value, min, max);
}

inline bool isNearlyEqual(float a, float b) { return ::tiny_skia::isNearlyEqual(a, b); }
inline bool isNearlyEqualWithinTolerance(float a, float b, float tolerance) {
  return ::tiny_skia::isNearlyEqualWithinTolerance(a, b, tolerance);
}
inline bool isNearlyZero(float value) { return ::tiny_skia::isNearlyZero(value); }
inline bool isNearlyZeroWithinTolerance(float value, float tolerance) {
  return ::tiny_skia::isNearlyZeroWithinTolerance(value, tolerance);
}

inline bool almostDequalUlps(float a, float b) {
  constexpr std::int32_t kUlpsEpsilon = 16;
  const auto aBits = f32As2sCompliment(a);
  const auto bBits = f32As2sCompliment(b);
  return aBits < bBits + kUlpsEpsilon && bBits < aBits + kUlpsEpsilon;
}

}  // namespace tiny_skia::pathScalarRs
