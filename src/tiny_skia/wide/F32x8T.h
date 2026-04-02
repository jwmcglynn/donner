#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace tiny_skia::wide {

class I32x8T;
class U32x8T;

class F32x8T {
 public:
  F32x8T() = default;
  explicit constexpr F32x8T(std::array<float, 8> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr F32x8T splat(float n) { return F32x8T({n, n, n, n, n, n, n, n}); }

  [[nodiscard]] constexpr std::array<float, 8> lanes() const { return lanes_; }

  [[nodiscard]] F32x8T floor() const;
  [[nodiscard]] F32x8T fract() const;
  [[nodiscard]] F32x8T normalize() const;

  [[nodiscard]] I32x8T toI32x8Bitcast() const;
  [[nodiscard]] U32x8T toU32x8Bitcast() const;

  [[nodiscard]] F32x8T cmpEq(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T cmpNe(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T cmpGe(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T cmpGt(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T cmpLe(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T cmpLt(const F32x8T& rhs) const;

  [[nodiscard]] F32x8T blend(const F32x8T& t, const F32x8T& f) const;

  [[nodiscard]] F32x8T abs() const;
  [[nodiscard]] F32x8T sqrt() const;
  [[nodiscard]] F32x8T recipFast() const;
  [[nodiscard]] F32x8T recipSqrt() const;
  [[nodiscard]] F32x8T powf(float exp) const;
  [[nodiscard]] F32x8T max(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T min(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T isFinite() const;
  [[nodiscard]] F32x8T round() const;

  [[nodiscard]] I32x8T roundInt() const;
  [[nodiscard]] I32x8T truncInt() const;

  [[nodiscard]] F32x8T operator+(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator-(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator*(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator/(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator&(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator|(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator^(const F32x8T& rhs) const;
  [[nodiscard]] F32x8T operator-() const;
  [[nodiscard]] F32x8T operator~() const;
  [[nodiscard]] bool operator==(const F32x8T& rhs) const;
  F32x8T& operator+=(const F32x8T& rhs);

 private:
  static constexpr float kTrueMask = std::bit_cast<float>(0xFFFFFFFFu);

  [[nodiscard]] static float cmpMask(bool predicate);

  std::array<float, 8> lanes_{};
};

}  // namespace tiny_skia::wide
