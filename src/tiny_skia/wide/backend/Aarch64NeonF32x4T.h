#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarF32x4T.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide::backend::aarch64_neon {

#if defined(__aarch64__) && defined(__ARM_NEON)

[[nodiscard]] inline float32x4_t loadF32x4(const std::array<float, 4>& lanes) {
  return vld1q_f32(lanes.data());
}

[[nodiscard]] inline std::array<float, 4> storeF32x4(float32x4_t value) {
  std::array<float, 4> out{};
  vst1q_f32(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 4> storeI32x4(int32x4_t value) {
  std::array<std::int32_t, 4> out{};
  vst1q_s32(out.data(), value);
  return out;
}

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  return storeF32x4(vabsq_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vmaxq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vminq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vceqq_f32(loadF32x4(lhs), loadF32x4(rhs))));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vmvnq_u32(vceqq_f32(loadF32x4(lhs), loadF32x4(rhs)))));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vcgeq_f32(loadF32x4(lhs), loadF32x4(rhs))));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vcgtq_f32(loadF32x4(lhs), loadF32x4(rhs))));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vcgeq_f32(loadF32x4(rhs), loadF32x4(lhs))));
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(vcgtq_f32(loadF32x4(rhs), loadF32x4(lhs))));
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  return storeF32x4(vreinterpretq_f32_u32(vbslq_u32(vreinterpretq_u32_f32(loadF32x4(mask)),
                                                    vreinterpretq_u32_f32(loadF32x4(t)),
                                                    vreinterpretq_u32_f32(loadF32x4(f)))));
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return storeF32x4(vrndmq_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return storeF32x4(vrndnq_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return storeI32x4(vcvtnq_s32_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return storeI32x4(vcvtq_s32_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  return storeI32x4(vreinterpretq_s32_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  const float32x4_t x = loadF32x4(lanes);
  float32x4_t approx = vrecpeq_f32(x);
  approx = vmulq_f32(vrecpsq_f32(x, approx), approx);
  return storeF32x4(approx);
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  const float32x4_t x = loadF32x4(lanes);
  float32x4_t approx = vrsqrteq_f32(x);
  approx = vmulq_f32(vrsqrtsq_f32(x, vmulq_f32(approx, approx)), approx);
  return storeF32x4(approx);
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return storeF32x4(vsqrtq_f32(loadF32x4(lanes)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vaddq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vsubq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vmulq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return storeF32x4(vdivq_f32(loadF32x4(lhs), loadF32x4(rhs)));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(
      vandq_u32(vreinterpretq_u32_f32(loadF32x4(lhs)), vreinterpretq_u32_f32(loadF32x4(rhs)))));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(
      vorrq_u32(vreinterpretq_u32_f32(loadF32x4(lhs)), vreinterpretq_u32_f32(loadF32x4(rhs)))));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return storeF32x4(vreinterpretq_f32_u32(
      veorq_u32(vreinterpretq_u32_f32(loadF32x4(lhs)), vreinterpretq_u32_f32(loadF32x4(rhs)))));
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  return storeF32x4(vreinterpretq_f32_u32(vmvnq_u32(vreinterpretq_u32_f32(loadF32x4(lanes)))));
}

#else

[[nodiscard]] inline std::array<float, 4> f32x4Abs(const std::array<float, 4>& lanes) {
  return scalar::f32x4Abs(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Max(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Max(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Min(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Min(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpEq(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpNe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpGt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLe(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4CmpLt(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Blend(const std::array<float, 4>& mask,
                                                     const std::array<float, 4>& t,
                                                     const std::array<float, 4>& f) {
  return scalar::f32x4Blend(mask, t, f);
}

[[nodiscard]] inline std::array<float, 4> f32x4Floor(const std::array<float, 4>& lanes) {
  return scalar::f32x4Floor(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Round(const std::array<float, 4>& lanes) {
  return scalar::f32x4Round(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4RoundInt(const std::array<float, 4>& lanes) {
  return scalar::f32x4RoundInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4TruncInt(const std::array<float, 4>& lanes) {
  return scalar::f32x4TruncInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 4> f32x4ToI32Bitcast(
    const std::array<float, 4>& lanes) {
  return scalar::f32x4ToI32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipFast(const std::array<float, 4>& lanes) {
  return scalar::f32x4RecipFast(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4RecipSqrt(const std::array<float, 4>& lanes) {
  return scalar::f32x4RecipSqrt(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Sqrt(const std::array<float, 4>& lanes) {
  return scalar::f32x4Sqrt(lanes);
}

[[nodiscard]] inline std::array<float, 4> f32x4Add(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Add(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Sub(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Sub(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Mul(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4Div(const std::array<float, 4>& lhs,
                                                   const std::array<float, 4>& rhs) {
  return scalar::f32x4Div(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitAnd(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return scalar::f32x4BitAnd(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitOr(const std::array<float, 4>& lhs,
                                                     const std::array<float, 4>& rhs) {
  return scalar::f32x4BitOr(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitXor(const std::array<float, 4>& lhs,
                                                      const std::array<float, 4>& rhs) {
  return scalar::f32x4BitXor(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 4> f32x4BitNot(const std::array<float, 4>& lanes) {
  return scalar::f32x4BitNot(lanes);
}

#endif

}  // namespace tiny_skia::wide::backend::aarch64_neon
