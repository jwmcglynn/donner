#pragma once

#include <array>
#include <cstdint>

namespace tiny_skia::wide {

class F32x8T;
class U32x8T;

class I32x8T {
 public:
  I32x8T() = default;
  explicit constexpr I32x8T(std::array<std::int32_t, 8> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr I32x8T splat(std::int32_t n) {
    return I32x8T({n, n, n, n, n, n, n, n});
  }

  [[nodiscard]] constexpr std::array<std::int32_t, 8> lanes() const { return lanes_; }

  [[nodiscard]] I32x8T blend(const I32x8T& t, const I32x8T& f) const;

  [[nodiscard]] I32x8T cmpEq(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T cmpGt(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T cmpLt(const I32x8T& rhs) const;

  [[nodiscard]] F32x8T toF32x8() const;
  [[nodiscard]] U32x8T toU32x8Bitcast() const;
  [[nodiscard]] F32x8T toF32x8Bitcast() const;

  [[nodiscard]] I32x8T operator+(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T operator*(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T operator&(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T operator|(const I32x8T& rhs) const;
  [[nodiscard]] I32x8T operator^(const I32x8T& rhs) const;

 private:
  static constexpr std::int32_t kTrueMask = -1;

  [[nodiscard]] static constexpr std::int32_t cmpMask(bool predicate) {
    return predicate ? kTrueMask : 0;
  }

  std::array<std::int32_t, 8> lanes_{};
};

}  // namespace tiny_skia::wide
