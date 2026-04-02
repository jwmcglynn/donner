#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/Wide.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline float f32x8CmpMask(bool predicate) {
  return predicate ? std::bit_cast<float>(0xFFFFFFFFu) : 0.0f;
}

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::floor(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<std::int32_t>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<std::uint32_t>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] == rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] != rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] >= rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] > rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] <= rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = f32x8CmpMask(lhs[i] < rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t maskBits = std::bit_cast<std::uint32_t>(mask[i]);
    const std::uint32_t tBits = std::bit_cast<std::uint32_t>(t[i]);
    const std::uint32_t fBits = std::bit_cast<std::uint32_t>(f[i]);
    out[i] = std::bit_cast<float>(genericBitBlend(maskBits, tBits, fBits));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::fabs(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::sqrt(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 1.0f / lanes[i];
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = 1.0f / std::sqrt(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Powf(const std::array<float, 8>& lanes, float exp) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::pow(lanes[i], exp);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = fasterMax(lhs[i], rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = fasterMin(lhs[i], rhs[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::nearbyint(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::int32_t>(std::nearbyint(lanes[i]));
  }

  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::int32_t>(std::trunc(lanes[i]));
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] + rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] - rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] * rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] / rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<float>(lhsBits & rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<float>(lhsBits | rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t lhsBits = std::bit_cast<std::uint32_t>(lhs[i]);
    const std::uint32_t rhsBits = std::bit_cast<std::uint32_t>(rhs[i]);
    out[i] = std::bit_cast<float>(lhsBits ^ rhsBits);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(lanes[i]);
    out[i] = std::bit_cast<float>(~bits);
  }

  return out;
}

}  // namespace tiny_skia::wide::backend::scalar
