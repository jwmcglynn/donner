#pragma once

/// @file FloatingPoint.h
/// @brief Validated floating-point types: NormalizedF32 [0,1], FiniteF32, etc.

#include <cstdint>
#include <optional>

namespace tiny_skia {

/// A float guaranteed to be in [0, 1].
class NormalizedF32 {
 public:
  static const NormalizedF32 ZERO;
  static const NormalizedF32 ONE;

  constexpr NormalizedF32() = default;
  explicit constexpr NormalizedF32(float value) : value_(value) {}

  static constexpr NormalizedF32 newUnchecked(float value) { return NormalizedF32(value); }

  /// Returns nullopt if outside [0,1] or non-finite.
  [[nodiscard]] static std::optional<NormalizedF32> create(float value) { return newFloat(value); }

  [[nodiscard]] static std::optional<NormalizedF32> newFloat(float value);
  /// Clamps to [0,1].
  [[nodiscard]] static NormalizedF32 newClamped(float value);
  [[nodiscard]] static NormalizedF32 fromU8(std::uint8_t value);

  [[nodiscard]] constexpr float get() const { return value_; }

  constexpr bool operator==(const NormalizedF32&) const = default;
  constexpr bool operator<(const NormalizedF32& o) const { return value_ < o.value_; }
  constexpr bool operator<=(const NormalizedF32& o) const { return value_ <= o.value_; }
  constexpr bool operator>(const NormalizedF32& o) const { return value_ > o.value_; }
  constexpr bool operator>=(const NormalizedF32& o) const { return value_ >= o.value_; }

  [[nodiscard]] static constexpr NormalizedF32 zero() { return NormalizedF32(0.0f); }
  [[nodiscard]] static constexpr NormalizedF32 one() { return NormalizedF32(1.0f); }

 private:
  float value_ = 0.0f;
};

inline constexpr NormalizedF32 NormalizedF32::ZERO = NormalizedF32(0.0f);
inline constexpr NormalizedF32 NormalizedF32::ONE = NormalizedF32(1.0f);

/// @internal
/// A float in (0, 1) exclusive.
class NormalizedF32Exclusive {
 public:
  static const NormalizedF32Exclusive ANY;
  static const NormalizedF32Exclusive HALF;

  [[nodiscard]] static std::optional<NormalizedF32Exclusive> create(float v);
  [[nodiscard]] static NormalizedF32Exclusive newBounded(float v);

  [[nodiscard]] constexpr float get() const { return value_; }
  [[nodiscard]] NormalizedF32 toNormalized() const;

  constexpr bool operator==(const NormalizedF32Exclusive&) const = default;
  constexpr bool operator<(const NormalizedF32Exclusive& o) const { return value_ < o.value_; }

 private:
  explicit constexpr NormalizedF32Exclusive(float v) : value_(v) {}
  float value_ = 0.5f;
};

inline constexpr NormalizedF32Exclusive NormalizedF32Exclusive::ANY = NormalizedF32Exclusive(0.5f);
inline constexpr NormalizedF32Exclusive NormalizedF32Exclusive::HALF = NormalizedF32Exclusive(0.5f);

/// @internal
/// A float guaranteed to be > 0 and finite.
class NonZeroPositiveF32 {
 public:
  [[nodiscard]] static std::optional<NonZeroPositiveF32> create(float v);
  [[nodiscard]] constexpr float get() const { return value_; }

 private:
  explicit constexpr NonZeroPositiveF32(float v) : value_(v) {}
  float value_ = 1.0f;
};

/// @internal
/// A float guaranteed to be finite.
class FiniteF32 {
 public:
  [[nodiscard]] static std::optional<FiniteF32> create(float v);
  [[nodiscard]] constexpr float get() const { return value_; }

 private:
  explicit constexpr FiniteF32(float v) : value_(v) {}
  float value_ = 0.0f;
};

/// @internal
[[nodiscard]] std::int32_t saturateCastI32(float x);
/// @internal
[[nodiscard]] std::int32_t saturateCastI32(double x);
/// @internal
[[nodiscard]] std::int32_t saturateFloorToI32(float x);
/// @internal
[[nodiscard]] std::int32_t saturateCeilToI32(float x);
/// @internal
[[nodiscard]] std::int32_t saturateRoundToI32(float x);
/// @internal
[[nodiscard]] std::int32_t f32As2sCompliment(float x);

}  // namespace tiny_skia
