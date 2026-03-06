#include "tiny_skia/FixedPoint.h"

#include <cassert>
#include <cstdlib>
#include <limits>

#include "tiny_skia/FloatingPoint.h"

namespace tiny_skia {

FDot6 fdot6::fromI32(std::int32_t n) {
  assert(static_cast<std::int16_t>(n) == n);
  return static_cast<FDot6>(n << 6);
}

FDot6 fdot6::fromF32(float n) { return static_cast<FDot6>(n * 64.0f); }

FDot6 fdot6::floor(FDot6 n) { return n >> 6; }

FDot6 fdot6::ceil(FDot6 n) { return (n + 63) >> 6; }

FDot6 fdot6::round(FDot6 n) { return (n + 32) >> 6; }

FDot16 fdot6::toFdot16(FDot6 n) {
  assert((leftShift(n, 10) >> 10) == n);
  return leftShift(n, 10);
}

FDot16 fdot6::div(FDot6 a, FDot6 b) {
  assert(b != 0);
  if (static_cast<std::int16_t>(a) == a) {
    return leftShift(a, 16) / b;
  }
  return fdot16::divide(a, b);
}

bool fdot6::canConvertToFdot16(FDot6 n) {
  const auto maxDot6 = std::numeric_limits<std::int32_t>::max() >> 10;
  return n >= -maxDot6 && n <= maxDot6;
}

std::uint8_t fdot6::smallScale(std::uint8_t value, FDot6 dot6) {
  assert(dot6 >= 0);
  assert(static_cast<std::uint32_t>(dot6) <= 64);
  return static_cast<std::uint8_t>((static_cast<std::int32_t>(value) * dot6) >> 6);
}

FDot8 fdot8::fromFdot16(FDot16 x) { return (x + 0x80) >> 8; }

FDot16 fdot16::fromF32(float x) { return saturateCastI32(x * static_cast<float>(fdot16::one)); }

std::int32_t fdot16::floorToI32(FDot16 x) { return x >> 16; }

std::int32_t fdot16::ceilToI32(FDot16 x) { return (x + one - 1) >> 16; }

std::int32_t fdot16::roundToI32(FDot16 x) { return (x + half) >> 16; }

FDot16 fdot16::mul(FDot16 a, FDot16 b) {
  return static_cast<FDot16>((static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)) >> 16);
}

FDot16 fdot16::divide(FDot6 numer, FDot6 denom) {
  const auto shifted = leftShift64(static_cast<long long>(numer), 16);
  const auto value = shifted / static_cast<long long>(denom);
  const auto bounded = bound<std::int64_t>(std::numeric_limits<std::int32_t>::min(), value,
                                           std::numeric_limits<std::int32_t>::max());
  return static_cast<FDot16>(bounded);
}

FDot16 fdot16::fastDiv(FDot6 a, FDot6 b) {
  assert((leftShift(a, 16) >> 16) == a);
  assert(b != 0);
  return leftShift(a, 16) / b;
}

}  // namespace tiny_skia
