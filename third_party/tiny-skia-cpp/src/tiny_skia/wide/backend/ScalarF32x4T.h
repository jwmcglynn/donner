#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/Wide.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline float f32x4CmpMask(bool predicate) {
  return predicate ? std::bit_cast<float>(0xFFFFFFFFu) : 0.0f;
}

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  return {std::fabs(lanes[0]), std::fabs(lanes[1]), std::fabs(lanes[2]), std::fabs(lanes[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {fasterMax(lhs[0], rhs[0]), fasterMax(lhs[1], rhs[1]), fasterMax(lhs[2], rhs[2]),
          fasterMax(lhs[3], rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {fasterMin(lhs[0], rhs[0]), fasterMin(lhs[1], rhs[1]), fasterMin(lhs[2], rhs[2]),
          fasterMin(lhs[3], rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] == rhs[0]), f32x4CmpMask(lhs[1] == rhs[1]),
          f32x4CmpMask(lhs[2] == rhs[2]), f32x4CmpMask(lhs[3] == rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] != rhs[0]), f32x4CmpMask(lhs[1] != rhs[1]),
          f32x4CmpMask(lhs[2] != rhs[2]), f32x4CmpMask(lhs[3] != rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] >= rhs[0]), f32x4CmpMask(lhs[1] >= rhs[1]),
          f32x4CmpMask(lhs[2] >= rhs[2]), f32x4CmpMask(lhs[3] >= rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] > rhs[0]), f32x4CmpMask(lhs[1] > rhs[1]),
          f32x4CmpMask(lhs[2] > rhs[2]), f32x4CmpMask(lhs[3] > rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] <= rhs[0]), f32x4CmpMask(lhs[1] <= rhs[1]),
          f32x4CmpMask(lhs[2] <= rhs[2]), f32x4CmpMask(lhs[3] <= rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return {f32x4CmpMask(lhs[0] < rhs[0]), f32x4CmpMask(lhs[1] < rhs[1]),
          f32x4CmpMask(lhs[2] < rhs[2]), f32x4CmpMask(lhs[3] < rhs[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  std::array<float, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t maskBits = std::bit_cast<std::uint32_t>(mask[i]);
    const std::uint32_t tBits = std::bit_cast<std::uint32_t>(t[i]);
    const std::uint32_t fBits = std::bit_cast<std::uint32_t>(f[i]);
    out[i] = std::bit_cast<float>(genericBitBlend(maskBits, tBits, fBits));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return {std::floor(lanes[0]), std::floor(lanes[1]), std::floor(lanes[2]), std::floor(lanes[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return {std::nearbyint(lanes[0]), std::nearbyint(lanes[1]), std::nearbyint(lanes[2]),
          std::nearbyint(lanes[3])};
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return {
      static_cast<std::int32_t>(std::nearbyint(lanes[0])),
      static_cast<std::int32_t>(std::nearbyint(lanes[1])),
      static_cast<std::int32_t>(std::nearbyint(lanes[2])),
      static_cast<std::int32_t>(std::nearbyint(lanes[3])),
  };
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return {
      static_cast<std::int32_t>(std::trunc(lanes[0])),
      static_cast<std::int32_t>(std::trunc(lanes[1])),
      static_cast<std::int32_t>(std::trunc(lanes[2])),
      static_cast<std::int32_t>(std::trunc(lanes[3])),
  };
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  return {
      std::bit_cast<std::int32_t>(lanes[0]),
      std::bit_cast<std::int32_t>(lanes[1]),
      std::bit_cast<std::int32_t>(lanes[2]),
      std::bit_cast<std::int32_t>(lanes[3]),
  };
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  return {1.0f / lanes[0], 1.0f / lanes[1], 1.0f / lanes[2], 1.0f / lanes[3]};
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  return {1.0f / std::sqrt(lanes[0]), 1.0f / std::sqrt(lanes[1]), 1.0f / std::sqrt(lanes[2]),
          1.0f / std::sqrt(lanes[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return {std::sqrt(lanes[0]), std::sqrt(lanes[1]), std::sqrt(lanes[2]), std::sqrt(lanes[3])};
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2], lhs[3] + rhs[3]};
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2], lhs[3] - rhs[3]};
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {lhs[0] * rhs[0], lhs[1] * rhs[1], lhs[2] * rhs[2], lhs[3] * rhs[3]};
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return {lhs[0] / rhs[0], lhs[1] / rhs[1], lhs[2] / rhs[2], lhs[3] / rhs[3]};
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  std::array<float, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(lhs[i]) &
                                  std::bit_cast<std::uint32_t>(rhs[i]));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  std::array<float, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(lhs[i]) |
                                  std::bit_cast<std::uint32_t>(rhs[i]));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  std::array<float, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(std::bit_cast<std::uint32_t>(lhs[i]) ^
                                  std::bit_cast<std::uint32_t>(rhs[i]));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  std::array<float, 4> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(~std::bit_cast<std::uint32_t>(lanes[i]));
  }

  return out;
}

}  // namespace tiny_skia::wide::backend::scalar
