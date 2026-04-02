#pragma once

#include "tiny_skia/path64/Scalar64.h"

namespace tiny_skia::path64 {

constexpr double kDblEpsilonErr = scalar64::kDblEpsilonErr;
constexpr double kFloatEpsilonHalf = scalar64::kFloatEpsilonHalf;
constexpr double kFloatEpsilonCubed = scalar64::kFloatEpsilonCubed;
constexpr double kFloatEpsilonInverse = scalar64::kFloatEpsilonInverse;

inline double bound(double value, double min, double max) {
  return scalar64::bound(min, value, max);
}

inline bool between(double value, double a, double b) { return scalar64::between(a, b, value); }

inline bool preciselyZero(double value) { return scalar64::preciselyZero(value); }

inline bool approximatelyZeroOrMore(double value) {
  return scalar64::approximatelyZeroOrMore(value);
}

inline bool approximatelyOneOrLess(double value) { return scalar64::approximatelyOneOrLess(value); }

inline bool approximatelyZero(double value) { return scalar64::approximatelyZero(value); }

inline bool approximatelyZeroInverse(double value) {
  return scalar64::approximatelyZeroInverse(value);
}

inline bool approximatelyZeroCubed(double value) { return scalar64::approximatelyZeroCubed(value); }

inline bool approximatelyZeroHalf(double value) { return scalar64::approximatelyZeroHalf(value); }

inline bool approximatelyZeroWhenComparedTo(double value, double other) {
  return scalar64::approximatelyZeroWhenComparedTo(value, other);
}

inline bool approximatelyEqual(double value, double other) {
  return scalar64::approximatelyEqual(value, other);
}

inline bool approximatelyEqualHalf(double value, double other) {
  return scalar64::approximatelyEqualHalf(value, other);
}

inline bool almostDequalUlps(double value, double other) {
  return scalar64::almostDequalUlps(value, other);
}

double cubeRoot(double value);

double interp(double a, double b, double t);

}  // namespace tiny_skia::path64
