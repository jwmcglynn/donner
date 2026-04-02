#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/Wide.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline constexpr std::int32_t i32x8CmpMask(bool predicate) {
  return predicate ? -1 : 0;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Blend(const std::array<std::int32_t, 8>& mask,
                                                            const std::array<std::int32_t, 8>& t,
                                                            const std::array<std::int32_t, 8>& f) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t maskBits = std::bit_cast<std::uint32_t>(mask[i]);
    const std::uint32_t tBits = std::bit_cast<std::uint32_t>(t[i]);
    const std::uint32_t fBits = std::bit_cast<std::uint32_t>(f[i]);
    out[i] = std::bit_cast<std::int32_t>(genericBitBlend(maskBits, tBits, fBits));
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpEq(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = i32x8CmpMask(lhs[i] == rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpGt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = i32x8CmpMask(lhs[i] > rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8CmpLt(
    const std::array<std::int32_t, 8>& lhs, const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = i32x8CmpMask(lhs[i] < rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32(const std::array<std::int32_t, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<float>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> i32x8ToU32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<std::uint32_t>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> i32x8ToF32Bitcast(
    const std::array<std::int32_t, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Add(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<std::int32_t>(lhsBits + rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Mul(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<std::int32_t>(lhsBits * rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8And(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] & rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Or(const std::array<std::int32_t, 8>& lhs,
                                                         const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] | rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> i32x8Xor(const std::array<std::int32_t, 8>& lhs,
                                                          const std::array<std::int32_t, 8>& rhs) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] ^ rhs[i];
  }

  return out;
}

}  // namespace tiny_skia::wide::backend::scalar
