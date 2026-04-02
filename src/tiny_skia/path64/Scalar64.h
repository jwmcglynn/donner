#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tiny_skia::path64 {

namespace scalar64 {

constexpr double kDblEpsilonErr = std::numeric_limits<double>::epsilon() * 4.0;
constexpr double kFloatEpsilonHalf = (std::numeric_limits<float>::epsilon() / 2.0);
constexpr double kFloatEpsilonCubed = static_cast<double>(std::numeric_limits<float>::epsilon() *
                                                          std::numeric_limits<float>::epsilon() *
                                                          std::numeric_limits<float>::epsilon());
constexpr double kFloatEpsilonInverse =
    1.0 / static_cast<double>(std::numeric_limits<float>::epsilon());
constexpr float kScalarMaxF = std::numeric_limits<float>::max();

constexpr double bound(double min, double value, double max) {
  return std::max(min, std::min(value, max));
}

constexpr bool between(double a, double b, double value) {
  return (a - value) * (b - value) <= 0.0 || (value == 0.0 && a == 0.0 && b == 0.0);
}

constexpr bool approximatelyZero(double value) {
  return std::abs(value) < std::numeric_limits<double>::epsilon();
}

constexpr bool preciselyZero(double value) { return std::abs(value) < kDblEpsilonErr; }

constexpr bool approximatelyZeroOrMore(double value) {
  return value > -std::numeric_limits<double>::epsilon();
}

constexpr bool approximatelyOneOrLess(double value) {
  return value < 1.0 + std::numeric_limits<double>::epsilon();
}

constexpr bool approximatelyZeroInverse(double value) {
  return std::abs(value) > kFloatEpsilonInverse;
}

constexpr bool approximatelyZeroCubed(double value) {
  return std::abs(value) < kFloatEpsilonCubed;
};

constexpr bool approximatelyZeroHalf(double value) { return value < kFloatEpsilonHalf; }

constexpr bool approximatelyZeroWhenComparedTo(double value, double other) {
  return value == 0.0 ||
         std::abs(value) < (other * static_cast<double>(std::numeric_limits<float>::epsilon()));
}

constexpr bool approximatelyEqual(double value, double other) {
  return (value - other) < std::numeric_limits<double>::epsilon() &&
         (other - value) < std::numeric_limits<double>::epsilon();
}

constexpr bool approximatelyEqualHalf(double value, double other) {
  return (value - other) < kFloatEpsilonHalf;
}

inline bool almostDequalUlps(double value, double other) {
  if (std::abs(value) < kScalarMaxF && std::abs(other) < kScalarMaxF) {
    const auto a = static_cast<float>(value);
    const auto b = static_cast<float>(other);
    return std::abs(a - b) <= std::abs(a) * 16.0f * std::numeric_limits<float>::epsilon();
  }
  return std::abs(value - other) / std::max(std::abs(value), std::abs(other)) <
         static_cast<double>(std::numeric_limits<float>::epsilon() * 16.0f);
}

double cubeRoot(double value);

}  // namespace scalar64

}  // namespace tiny_skia::path64
