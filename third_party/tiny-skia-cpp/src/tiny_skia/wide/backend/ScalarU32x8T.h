#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline std::array<std::int32_t, 8> u32x8ToI32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  std::array<std::int32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<std::int32_t>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<float, 8> u32x8ToF32Bitcast(
    const std::array<std::uint32_t, 8>& lanes) {
  std::array<float, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::bit_cast<float>(lanes[i]);
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpEq(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] == rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpNe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] != rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] < rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpLe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] <= rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGt(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] > rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8CmpGe(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] >= rhs[i] ? UINT32_MAX : 0;
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Not(
    const std::array<std::uint32_t, 8>& lanes) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = ~lanes[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Add(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] + rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8And(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] & rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Or(const std::array<std::uint32_t, 8>& lhs,
                                                          const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] | rhs[i];
  }

  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> u32x8Xor(
    const std::array<std::uint32_t, 8>& lhs, const std::array<std::uint32_t, 8>& rhs) {
  std::array<std::uint32_t, 8> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = lhs[i] ^ rhs[i];
  }

  return out;
}

}  // namespace tiny_skia::wide::backend::scalar
