// Copyright 2020 Yevhenii Reizner (Rust original)
// Copyright 2026 tiny-skia-cpp contributors (C++ port)
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cmath>

namespace tiny_skia {

/// A quartet of f32 numbers.
struct F32x4 {
  float a = 0.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 0.0f;

  [[nodiscard]] static constexpr F32x4 from(float pa, float pb, float pc, float pd) {
    return F32x4{pa, pb, pc, pd};
  }

  [[nodiscard]] F32x4 max(F32x4 rhs) const {
    return F32x4{std::fmax(a, rhs.a), std::fmax(b, rhs.b), std::fmax(c, rhs.c),
                 std::fmax(d, rhs.d)};
  }

  [[nodiscard]] F32x4 min(F32x4 rhs) const {
    return F32x4{std::fmin(a, rhs.a), std::fmin(b, rhs.b), std::fmin(c, rhs.c),
                 std::fmin(d, rhs.d)};
  }

  constexpr F32x4& operator+=(F32x4 rhs) {
    a += rhs.a;
    b += rhs.b;
    c += rhs.c;
    d += rhs.d;
    return *this;
  }

  constexpr F32x4& operator*=(F32x4 rhs) {
    a *= rhs.a;
    b *= rhs.b;
    c *= rhs.c;
    d *= rhs.d;
    return *this;
  }

  constexpr bool operator==(const F32x4&) const = default;
};

[[nodiscard]] inline constexpr F32x4 operator+(F32x4 lhs, F32x4 rhs) {
  return F32x4{lhs.a + rhs.a, lhs.b + rhs.b, lhs.c + rhs.c, lhs.d + rhs.d};
}

[[nodiscard]] inline constexpr F32x4 operator-(F32x4 lhs, F32x4 rhs) {
  return F32x4{lhs.a - rhs.a, lhs.b - rhs.b, lhs.c - rhs.c, lhs.d - rhs.d};
}

[[nodiscard]] inline constexpr F32x4 operator*(F32x4 lhs, F32x4 rhs) {
  return F32x4{lhs.a * rhs.a, lhs.b * rhs.b, lhs.c * rhs.c, lhs.d * rhs.d};
}

}  // namespace tiny_skia
