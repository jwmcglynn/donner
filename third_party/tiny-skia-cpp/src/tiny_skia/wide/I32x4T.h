#pragma once

#include <array>
#include <cstdint>

namespace tiny_skia::wide {

class F32x4T;

class I32x4T {
 public:
  I32x4T() = default;
  explicit constexpr I32x4T(std::array<std::int32_t, 4> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr I32x4T splat(std::int32_t n) { return I32x4T({n, n, n, n}); }

  [[nodiscard]] constexpr std::array<std::int32_t, 4> lanes() const { return lanes_; }

  [[nodiscard]] I32x4T blend(const I32x4T& t, const I32x4T& f) const;

  [[nodiscard]] I32x4T cmpEq(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T cmpGt(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T cmpLt(const I32x4T& rhs) const;

  [[nodiscard]] F32x4T toF32x4() const;
  [[nodiscard]] F32x4T toF32x4Bitcast() const;

  [[nodiscard]] I32x4T operator+(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T operator*(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T operator&(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T operator|(const I32x4T& rhs) const;
  [[nodiscard]] I32x4T operator^(const I32x4T& rhs) const;

 private:
  static constexpr std::int32_t kTrueMask = -1;
  [[nodiscard]] static constexpr std::int32_t cmpMask(bool predicate) {
    return predicate ? kTrueMask : 0;
  }

  std::array<std::int32_t, 4> lanes_{};
};

}  // namespace tiny_skia::wide
