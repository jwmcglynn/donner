#pragma once

#include <array>
#include <cstdint>

namespace tiny_skia::wide {

class U32x4T {
 public:
  U32x4T() = default;
  explicit constexpr U32x4T(std::array<std::uint32_t, 4> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr U32x4T splat(std::uint32_t n) { return U32x4T({n, n, n, n}); }

  [[nodiscard]] constexpr std::array<std::uint32_t, 4> lanes() const { return lanes_; }

  [[nodiscard]] U32x4T cmpEq(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T cmpNe(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T cmpLt(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T cmpLe(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T cmpGt(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T cmpGe(const U32x4T& rhs) const;

  template <int Rhs>
  [[nodiscard]] U32x4T shl() const {
    static_assert(Rhs >= 0);
    return U32x4T({lanes_[0] << static_cast<std::uint32_t>(Rhs),
                   lanes_[1] << static_cast<std::uint32_t>(Rhs),
                   lanes_[2] << static_cast<std::uint32_t>(Rhs),
                   lanes_[3] << static_cast<std::uint32_t>(Rhs)});
  }

  template <int Rhs>
  [[nodiscard]] U32x4T shr() const {
    static_assert(Rhs >= 0);
    return U32x4T({lanes_[0] >> static_cast<std::uint32_t>(Rhs),
                   lanes_[1] >> static_cast<std::uint32_t>(Rhs),
                   lanes_[2] >> static_cast<std::uint32_t>(Rhs),
                   lanes_[3] >> static_cast<std::uint32_t>(Rhs)});
  }

  [[nodiscard]] U32x4T operator~() const;
  [[nodiscard]] U32x4T operator+(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T operator&(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T operator|(const U32x4T& rhs) const;
  [[nodiscard]] U32x4T operator^(const U32x4T& rhs) const;

 private:
  std::array<std::uint32_t, 4> lanes_{};
};

}  // namespace tiny_skia::wide
