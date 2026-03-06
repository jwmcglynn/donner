#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarI32x4T.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide::backend::aarch64_neon {

#if defined(__aarch64__) && defined(__ARM_NEON)

[[nodiscard]] inline int32x4_t loadI32x4(const std::array<std::int32_t, 4>& lanes) {
  return vld1q_s32(lanes.data());
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(int32x4_t value) {
  std::array<std::int32_t, 4> out{};
  vst1q_s32(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(float32x4_t value) {
  std::array<float, 4> out{};
  vst1q_f32(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  return storeI32x4(vbslq_s32(vreinterpretq_u32_s32(loadI32x4(mask)), loadI32x4(t), loadI32x4(f)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vreinterpretq_s32_u32(vceqq_s32(loadI32x4(lhs), loadI32x4(rhs))));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vreinterpretq_s32_u32(vcgtq_s32(loadI32x4(lhs), loadI32x4(rhs))));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vreinterpretq_s32_u32(vcgtq_s32(loadI32x4(rhs), loadI32x4(lhs))));
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return storeF32x4(vcvtq_f32_s32(loadI32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vaddq_s32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vmulq_s32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vandq_s32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(vorrq_s32(loadI32x4(lhs), loadI32x4(rhs)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return storeI32x4(veorq_s32(loadI32x4(lhs), loadI32x4(rhs)));
}

#else

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Blend(const std::array<std::int32_t, 4>& mask,
                                                            const std::array<std::int32_t, 4>& t,
                                                            const std::array<std::int32_t, 4>& f) {
  return scalar::i32x4Blend(mask, t, f);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpEq(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpGt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4CmpLt(
    const std::array<std::int32_t, 4>& lhs, const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> i32x4ToF32(const std::array<std::int32_t, 4>& lanes) {
  return scalar::i32x4ToF32(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Add(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Mul(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4And(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4And(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Or(const std::array<std::int32_t, 4>& lhs,
                                                         const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Or(lhs, rhs);
}

[[nodiscard]] inline std::array<std::int32_t, 4> i32x4Xor(const std::array<std::int32_t, 4>& lhs,
                                                          const std::array<std::int32_t, 4>& rhs) {
  return scalar::i32x4Xor(lhs, rhs);
}

#endif

}  // namespace tiny_skia::wide::backend::aarch64_neon
