#pragma once

#include <array>
#include <cstdint>

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpEq(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] == rhs[0] ? UINT32_MAX : 0,
      lhs[1] == rhs[1] ? UINT32_MAX : 0,
      lhs[2] == rhs[2] ? UINT32_MAX : 0,
      lhs[3] == rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpNe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] != rhs[0] ? UINT32_MAX : 0,
      lhs[1] != rhs[1] ? UINT32_MAX : 0,
      lhs[2] != rhs[2] ? UINT32_MAX : 0,
      lhs[3] != rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] < rhs[0] ? UINT32_MAX : 0,
      lhs[1] < rhs[1] ? UINT32_MAX : 0,
      lhs[2] < rhs[2] ? UINT32_MAX : 0,
      lhs[3] < rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] <= rhs[0] ? UINT32_MAX : 0,
      lhs[1] <= rhs[1] ? UINT32_MAX : 0,
      lhs[2] <= rhs[2] ? UINT32_MAX : 0,
      lhs[3] <= rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] > rhs[0] ? UINT32_MAX : 0,
      lhs[1] > rhs[1] ? UINT32_MAX : 0,
      lhs[2] > rhs[2] ? UINT32_MAX : 0,
      lhs[3] > rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {
      lhs[0] >= rhs[0] ? UINT32_MAX : 0,
      lhs[1] >= rhs[1] ? UINT32_MAX : 0,
      lhs[2] >= rhs[2] ? UINT32_MAX : 0,
      lhs[3] >= rhs[3] ? UINT32_MAX : 0,
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Not(
    const std::array<std::uint32_t, 4>& lanes) {
  return {~lanes[0], ~lanes[1], ~lanes[2], ~lanes[3]};
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Add(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2], lhs[3] + rhs[3]};
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4And(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {lhs[0] & rhs[0], lhs[1] & rhs[1], lhs[2] & rhs[2], lhs[3] & rhs[3]};
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Or(const std::array<std::uint32_t, 4>& lhs,
                                                          const std::array<std::uint32_t, 4>& rhs) {
  return {lhs[0] | rhs[0], lhs[1] | rhs[1], lhs[2] | rhs[2], lhs[3] | rhs[3]};
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Xor(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return {lhs[0] ^ rhs[0], lhs[1] ^ rhs[1], lhs[2] ^ rhs[2], lhs[3] ^ rhs[3]};
}

}  // namespace tiny_skia::wide::backend::scalar
