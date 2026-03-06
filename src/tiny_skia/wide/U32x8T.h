#pragma once

#include <array>
#include <cstdint>

namespace tiny_skia::wide {

class F32x8T;
class I32x8T;

class U32x8T {
 public:
  U32x8T() = default;
  explicit constexpr U32x8T(std::array<std::uint32_t, 8> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr U32x8T splat(std::uint32_t n) {
    return U32x8T({n, n, n, n, n, n, n, n});
  }

  [[nodiscard]] constexpr std::array<std::uint32_t, 8> lanes() const { return lanes_; }

  [[nodiscard]] I32x8T toI32x8Bitcast() const;
  [[nodiscard]] F32x8T toF32x8Bitcast() const;

  [[nodiscard]] U32x8T cmpEq(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T cmpNe(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T cmpLt(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T cmpLe(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T cmpGt(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T cmpGe(const U32x8T& rhs) const;

  template <int Rhs>
  [[nodiscard]] U32x8T shl() const {
    static_assert(Rhs >= 0);
    return U32x8T(
        {lanes_[0] << static_cast<std::uint32_t>(Rhs), lanes_[1] << static_cast<std::uint32_t>(Rhs),
         lanes_[2] << static_cast<std::uint32_t>(Rhs), lanes_[3] << static_cast<std::uint32_t>(Rhs),
         lanes_[4] << static_cast<std::uint32_t>(Rhs), lanes_[5] << static_cast<std::uint32_t>(Rhs),
         lanes_[6] << static_cast<std::uint32_t>(Rhs),
         lanes_[7] << static_cast<std::uint32_t>(Rhs)});
  }

  template <int Rhs>
  [[nodiscard]] U32x8T shr() const {
    static_assert(Rhs >= 0);
    return U32x8T(
        {lanes_[0] >> static_cast<std::uint32_t>(Rhs), lanes_[1] >> static_cast<std::uint32_t>(Rhs),
         lanes_[2] >> static_cast<std::uint32_t>(Rhs), lanes_[3] >> static_cast<std::uint32_t>(Rhs),
         lanes_[4] >> static_cast<std::uint32_t>(Rhs), lanes_[5] >> static_cast<std::uint32_t>(Rhs),
         lanes_[6] >> static_cast<std::uint32_t>(Rhs),
         lanes_[7] >> static_cast<std::uint32_t>(Rhs)});
  }

  [[nodiscard]] U32x8T operator~() const;
  [[nodiscard]] U32x8T operator+(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T operator&(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T operator|(const U32x8T& rhs) const;
  [[nodiscard]] U32x8T operator^(const U32x8T& rhs) const;

 private:
  std::array<std::uint32_t, 8> lanes_{};
};

}  // namespace tiny_skia::wide
