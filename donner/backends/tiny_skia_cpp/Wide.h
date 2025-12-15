#pragma once
/// @file

#include <array>

#include "donner/backends/tiny_skia_cpp/Color.h"

namespace donner::backends::tiny_skia_cpp {

/** Simple 4-lane float vector with optional SSE2/NEON acceleration. */
class alignas(16) F32x4 {
public:
  F32x4();

  /// Constructs a vector with all lanes set to the same value.
  static F32x4 Splat(float value);

  /// Loads a vector from an array of four values.
  static F32x4 FromArray(const std::array<float, 4>& values);

  /// Loads a vector from the RGBA components of a color.
  static F32x4 FromColor(const Color& color);

  /// Returns the underlying values as an array.
  std::array<float, 4> toArray() const;

  F32x4 operator+(const F32x4& rhs) const;
  F32x4 operator-(const F32x4& rhs) const;
  F32x4 operator*(const F32x4& rhs) const;
  F32x4 operator*(float scalar) const;
  F32x4 operator/(float scalar) const;

  F32x4& operator+=(const F32x4& rhs);

private:
  explicit F32x4(const std::array<float, 4>& values);
  std::array<float, 4> values_{};
};

}  // namespace donner::backends::tiny_skia_cpp

