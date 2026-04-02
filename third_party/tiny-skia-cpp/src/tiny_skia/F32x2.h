// Copyright 2020 Yevhenii Reizner (Rust original)
// Copyright 2026 tiny-skia-cpp contributors (C++ port)
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cmath>

namespace tiny_skia {

namespace pathF32x2Detail {

// A faster and more forgiving f32 max: unlike std, we do not care about NaN.
inline constexpr float pmax(float a, float b) { return a < b ? b : a; }

// A faster and more forgiving f32 min: unlike std, we do not care about NaN.
inline constexpr float pmin(float a, float b) { return b < a ? b : a; }

}  // namespace pathF32x2Detail

/// A pair of f32 numbers.
struct F32x2 {
  float x = 0.0f;
  float y = 0.0f;

  [[nodiscard]] static constexpr F32x2 from(float px, float py) { return F32x2{px, py}; }
  [[nodiscard]] static constexpr F32x2 splat(float v) { return F32x2{v, v}; }

  [[nodiscard]] F32x2 abs() const { return F32x2{std::abs(x), std::abs(y)}; }

  [[nodiscard]] constexpr F32x2 min(F32x2 other) const {
    return F32x2{pathF32x2Detail::pmin(x, other.x), pathF32x2Detail::pmin(y, other.y)};
  }

  [[nodiscard]] constexpr F32x2 max(F32x2 other) const {
    return F32x2{pathF32x2Detail::pmax(x, other.x), pathF32x2Detail::pmax(y, other.y)};
  }

  [[nodiscard]] constexpr float maxComponent() const { return pathF32x2Detail::pmax(x, y); }

  constexpr bool operator==(const F32x2&) const = default;
};

[[nodiscard]] inline constexpr F32x2 operator+(F32x2 a, F32x2 b) {
  return F32x2{a.x + b.x, a.y + b.y};
}

[[nodiscard]] inline constexpr F32x2 operator-(F32x2 a, F32x2 b) {
  return F32x2{a.x - b.x, a.y - b.y};
}

[[nodiscard]] inline constexpr F32x2 operator*(F32x2 a, F32x2 b) {
  return F32x2{a.x * b.x, a.y * b.y};
}

[[nodiscard]] inline constexpr F32x2 operator/(F32x2 a, F32x2 b) {
  return F32x2{a.x / b.x, a.y / b.y};
}

}  // namespace tiny_skia
