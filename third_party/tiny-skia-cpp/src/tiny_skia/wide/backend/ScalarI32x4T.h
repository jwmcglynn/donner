#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/Wide.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline constexpr std::int32_t i32x4CmpMask(bool predicate) {
  return predicate ? -1 : 0;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  std::array<std::int32_t, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t maskBits = std::bit_cast<std::uint32_t>(mask[i]);
    const std::uint32_t tBits = std::bit_cast<std::uint32_t>(t[i]);
    const std::uint32_t fBits = std::bit_cast<std::uint32_t>(f[i]);
    out[i] = std::bit_cast<std::int32_t>(genericBitBlend(maskBits, tBits, fBits));
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return {i32x4CmpMask(lhs[0] == rhs[0]), i32x4CmpMask(lhs[1] == rhs[1]),
          i32x4CmpMask(lhs[2] == rhs[2]), i32x4CmpMask(lhs[3] == rhs[3])};
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return {i32x4CmpMask(lhs[0] > rhs[0]), i32x4CmpMask(lhs[1] > rhs[1]),
          i32x4CmpMask(lhs[2] > rhs[2]), i32x4CmpMask(lhs[3] > rhs[3])};
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return {i32x4CmpMask(lhs[0] < rhs[0]), i32x4CmpMask(lhs[1] < rhs[1]),
          i32x4CmpMask(lhs[2] < rhs[2]), i32x4CmpMask(lhs[3] < rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return {static_cast<float>(lanes[0]), static_cast<float>(lanes[1]), static_cast<float>(lanes[2]),
          static_cast<float>(lanes[3])};
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32Bitcast(
    const std::array<std::int32_t, 4>& lanes) {
  return {std::bit_cast<float>(lanes[0]), std::bit_cast<float>(lanes[1]),
          std::bit_cast<float>(lanes[2]), std::bit_cast<float>(lanes[3])};
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  std::array<std::int32_t, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<std::int32_t>(lhsBits + rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  std::array<std::int32_t, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<std::int32_t>(lhsBits * rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return {lhs[0] & rhs[0], lhs[1] & rhs[1], lhs[2] & rhs[2], lhs[3] & rhs[3]};
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return {lhs[0] | rhs[0], lhs[1] | rhs[1], lhs[2] | rhs[2], lhs[3] | rhs[3]};
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return {lhs[0] ^ rhs[0], lhs[1] ^ rhs[1], lhs[2] ^ rhs[2], lhs[3] ^ rhs[3]};
}

}  // namespace tiny_skia::wide::backend::scalar
