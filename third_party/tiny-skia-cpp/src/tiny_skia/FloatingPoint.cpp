#include "tiny_skia/FloatingPoint.h"

#include <bit>
#include <cmath>
#include <limits>

namespace tiny_skia {

namespace {

constexpr float kFloatMax = 1.0f;
constexpr float kFloatMin = 0.0f;

constexpr float kMaxI32FitsInF32 = 2147483520.0f;
constexpr float kMinI32FitsInF32 = -kMaxI32FitsInF32;

constexpr float clamp01(float value) {
  return value < kFloatMin ? kFloatMin : (value > kFloatMax ? kFloatMax : value);
}

std::int32_t signBitTo2sCompliment(std::int32_t x) {
  if (x < 0) {
    x &= 0x7FFFFFFF;
    x = -x;
  }
  return x;
}

}  // namespace

std::optional<NormalizedF32> NormalizedF32::newFloat(float value) {
  if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
    return std::nullopt;
  }
  return NormalizedF32::newUnchecked(value);
}

NormalizedF32 NormalizedF32::newClamped(float value) {
  return NormalizedF32::newUnchecked(clamp01(value));
}

NormalizedF32 NormalizedF32::fromU8(std::uint8_t value) {
  return NormalizedF32::newUnchecked(static_cast<float>(value) * (1.0f / 255.0f));
}

std::optional<NormalizedF32Exclusive> NormalizedF32Exclusive::create(float v) {
  if (v > 0.0f && v < 1.0f) {
    return NormalizedF32Exclusive(v);
  }
  return std::nullopt;
}

NormalizedF32Exclusive NormalizedF32Exclusive::newBounded(float v) {
  float clamped = v;
  if (!(clamped > std::numeric_limits<float>::epsilon())) {
    clamped = std::numeric_limits<float>::epsilon();
  }
  if (clamped > 1.0f - std::numeric_limits<float>::epsilon()) {
    clamped = 1.0f - std::numeric_limits<float>::epsilon();
  }
  return NormalizedF32Exclusive(clamped);
}

NormalizedF32 NormalizedF32Exclusive::toNormalized() const {
  return NormalizedF32::newUnchecked(value_);
}

std::optional<NonZeroPositiveF32> NonZeroPositiveF32::create(float v) {
  if (v > 0.0f && std::isfinite(v)) {
    return NonZeroPositiveF32(v);
  }
  return std::nullopt;
}

std::optional<FiniteF32> FiniteF32::create(float v) {
  if (std::isfinite(v)) {
    return FiniteF32(v);
  }
  return std::nullopt;
}

std::int32_t saturateCastI32(float x) {
  x = x < kMaxI32FitsInF32 ? x : kMaxI32FitsInF32;
  x = x > kMinI32FitsInF32 ? x : kMinI32FitsInF32;
  return static_cast<std::int32_t>(x);
}

std::int32_t saturateCastI32(double x) {
  x = x < static_cast<double>(std::numeric_limits<std::int32_t>::max())
          ? x
          : static_cast<double>(std::numeric_limits<std::int32_t>::max());
  x = x > static_cast<double>(std::numeric_limits<std::int32_t>::min())
          ? x
          : static_cast<double>(std::numeric_limits<std::int32_t>::min());
  return static_cast<std::int32_t>(x);
}

std::int32_t saturateFloorToI32(float x) { return saturateCastI32(std::floor(x)); }

std::int32_t saturateCeilToI32(float x) { return saturateCastI32(std::ceil(x)); }

std::int32_t saturateRoundToI32(float x) { return saturateCastI32(std::floor(x) + 0.5f); }

std::int32_t f32As2sCompliment(float x) {
  return signBitTo2sCompliment(std::bit_cast<std::int32_t>(x));
}

}  // namespace tiny_skia
