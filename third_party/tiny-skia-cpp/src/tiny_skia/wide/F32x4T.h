#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace tiny_skia::wide {

class I32x4T;

class F32x4T {
 public:
  F32x4T() = default;
  explicit constexpr F32x4T(std::array<float, 4> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr F32x4T splat(float n) { return F32x4T({n, n, n, n}); }

  [[nodiscard]] constexpr std::array<float, 4> lanes() const { return lanes_; }

  [[nodiscard]] F32x4T abs() const;
  [[nodiscard]] F32x4T max(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T min(const F32x4T& rhs) const;

  [[nodiscard]] F32x4T cmpEq(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T cmpNe(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T cmpGe(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T cmpGt(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T cmpLe(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T cmpLt(const F32x4T& rhs) const;

  [[nodiscard]] F32x4T blend(const F32x4T& t, const F32x4T& f) const;

  [[nodiscard]] F32x4T floor() const;
  [[nodiscard]] F32x4T fract() const;
  [[nodiscard]] F32x4T normalize() const;
  [[nodiscard]] F32x4T round() const;
  [[nodiscard]] I32x4T roundInt() const;
  [[nodiscard]] I32x4T truncInt() const;

  [[nodiscard]] I32x4T toI32x4Bitcast() const;

  [[nodiscard]] F32x4T recipFast() const;
  [[nodiscard]] F32x4T recipSqrt() const;
  [[nodiscard]] F32x4T sqrt() const;

  [[nodiscard]] F32x4T operator+(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator-(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator*(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator/(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator&(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator|(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator^(const F32x4T& rhs) const;
  [[nodiscard]] F32x4T operator-() const;
  [[nodiscard]] F32x4T operator~() const;

  F32x4T& operator+=(const F32x4T& rhs);
  F32x4T& operator*=(const F32x4T& rhs);

  [[nodiscard]] bool operator==(const F32x4T& rhs) const;

 private:
  static constexpr float kTrueMask = std::bit_cast<float>(0xFFFFFFFFu);

  [[nodiscard]] static float cmpMask(bool predicate);

  std::array<float, 4> lanes_{};
};

}  // namespace tiny_skia::wide
