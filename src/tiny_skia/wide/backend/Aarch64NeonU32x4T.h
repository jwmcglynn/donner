#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarU32x4T.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide::backend::aarch64_neon {

#if defined(__aarch64__) && defined(__ARM_NEON)

[[nodiscard]] inline uint32x4_t loadU32x4(const std::array<std::uint32_t, 4>& lanes) {
  return vld1q_u32(lanes.data());
}

[[nodiscard]] inline std::array<std::uint32_t, 4> storeU32x4(uint32x4_t value) {
  std::array<std::uint32_t, 4> out{};
  vst1q_u32(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpEq(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vceqq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpNe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vmvnq_u32(vceqq_u32(loadU32x4(lhs), loadU32x4(rhs))));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vcgtq_u32(loadU32x4(rhs), loadU32x4(lhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vcgeq_u32(loadU32x4(rhs), loadU32x4(lhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vcgtq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vcgeq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Not(
    const std::array<std::uint32_t, 4>& lanes) {
  return storeU32x4(vmvnq_u32(loadU32x4(lanes)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Add(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vaddq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4And(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vandq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Or(const std::array<std::uint32_t, 4>& lhs,
                                                          const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(vorrq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Xor(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return storeU32x4(veorq_u32(loadU32x4(lhs), loadU32x4(rhs)));
}

#else

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpEq(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpNe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpLe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGt(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4CmpGe(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Not(
    const std::array<std::uint32_t, 4>& lanes) {
  return scalar::u32x4Not(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Add(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4And(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Or(const std::array<std::uint32_t, 4>& lhs,
                                                          const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::uint32_t, 4> u32x4Xor(
    const std::array<std::uint32_t, 4>& lhs, const std::array<std::uint32_t, 4>& rhs) {
  return scalar::u32x4Xor(lhs, rhs);
}

#endif

}  // namespace tiny_skia::wide::backend::aarch64_neon
