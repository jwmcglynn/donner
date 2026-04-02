#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/U16x16T.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline U16x16T u16x16Min(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::min(lhs.lanes()[i], rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Max(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::max(lhs.lanes()[i], rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16CmpLe(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs.lanes()[i] <= rhs.lanes()[i] ? UINT16_MAX : 0;
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Blend(const U16x16T& mask, const U16x16T& t, const U16x16T& e) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = genericBitBlend(mask.lanes()[i], t.lanes()[i], e.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Add(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] + rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Sub(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] - rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Mul(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] * rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Div(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] / rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16And(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] & rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Or(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] | rhs.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Not(const U16x16T& value) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(~value.lanes()[i]);
  }

  return U16x16T(out);
}

[[nodiscard]] inline U16x16T u16x16Shr(const U16x16T& lhs, const U16x16T& rhs) {
  std::array<std::uint16_t, 16> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint16_t>(lhs.lanes()[i] >> rhs.lanes()[i]);
  }

  return U16x16T(out);
}

}  // namespace tiny_skia::wide::backend::scalar
