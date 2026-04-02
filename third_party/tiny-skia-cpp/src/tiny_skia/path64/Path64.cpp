#include "tiny_skia/path64/Path64.h"

#include <bit>
#include <cstdint>

namespace tiny_skia::path64 {

namespace {

std::uint64_t asBits(double value) { return std::bit_cast<std::uint64_t>(value); }

double cbrt5d(double value) {
  constexpr std::uint32_t kCbrtBias = 715094163;
  const auto bits = asBits(value);
  const auto highBits = static_cast<std::uint32_t>(bits >> 32);
  const auto next = static_cast<std::uint64_t>(highBits / 3u + kCbrtBias);
  return std::bit_cast<double>(next << 32);
}

double cbrtaHalleyd(double a, double r) {
  const auto a3 = a * a * a;
  return a * ((a3 + r + r) / (a3 + a3 + r));
}

double halleyCbrt3d(double value) {
  double a = cbrt5d(value);
  a = cbrtaHalleyd(a, value);
  a = cbrtaHalleyd(a, value);
  return cbrtaHalleyd(a, value);
}

}  // namespace

double cubeRoot(double value) {
  if (scalar64::approximatelyZeroCubed(value)) {
    return 0.0;
  }

  const auto result = halleyCbrt3d(value < 0.0 ? -value : value);
  return value < 0.0 ? -result : result;
}

double interp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace tiny_skia::path64
