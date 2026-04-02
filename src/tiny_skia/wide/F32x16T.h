#pragma once

#include <cstdint>

#include "tiny_skia/wide/F32x8T.h"

namespace tiny_skia::wide {

class U16x16T;

class F32x16T {
 public:
  F32x16T() = default;
  constexpr F32x16T(F32x8T lo, F32x8T hi) : lo_(lo), hi_(hi) {}

  [[nodiscard]] static constexpr F32x16T splat(float n) {
    return F32x16T(F32x8T::splat(n), F32x8T::splat(n));
  }

  [[nodiscard]] constexpr F32x8T lo() const { return lo_; }
  [[nodiscard]] constexpr F32x8T hi() const { return hi_; }

  [[nodiscard]] F32x16T abs() const;
  [[nodiscard]] F32x16T cmpGt(const F32x16T& rhs) const;
  [[nodiscard]] F32x16T blend(const F32x16T& t, const F32x16T& f) const;
  [[nodiscard]] F32x16T normalize() const;
  [[nodiscard]] F32x16T floor() const;
  [[nodiscard]] F32x16T sqrt() const;
  [[nodiscard]] F32x16T round() const;

  void saveToU16x16(U16x16T& dst) const;

  [[nodiscard]] F32x16T operator+(const F32x16T& rhs) const;
  [[nodiscard]] F32x16T operator-(const F32x16T& rhs) const;
  [[nodiscard]] F32x16T operator*(const F32x16T& rhs) const;

 private:
  F32x8T lo_{};
  F32x8T hi_{};
};

}  // namespace tiny_skia::wide
