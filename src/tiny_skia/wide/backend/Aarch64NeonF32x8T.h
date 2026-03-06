#pragma once

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/ScalarF32x8T.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide::backend::aarch64_neon {

#if defined(__aarch64__) && defined(__ARM_NEON)

struct F32x8Neon {
  float32x4_t lo;
  float32x4_t hi;
};

[[nodiscard]] inline F32x8Neon loadF32x8(const std::array<float, 8>& lanes) {
  const float32x4x2_t v = vld1q_f32_x2(lanes.data());
  return F32x8Neon{v.val[0], v.val[1]};
}

[[nodiscard]] inline std::array<float, 8> storeF32x8(const F32x8Neon& value) {
  std::array<float, 8> out{};
  const float32x4x2_t v = {{value.lo, value.hi}};
  vst1q_f32_x2(out.data(), v);
  return out;
}

[[nodiscard]] inline std::array<std::int32_t, 8> storeI32x8(int32x4_t lo, int32x4_t hi) {
  std::array<std::int32_t, 8> out{};
  vst1q_s32(out.data(), lo);
  vst1q_s32(out.data() + 4, hi);
  return out;
}

[[nodiscard]] inline std::array<std::uint32_t, 8> storeU32x8(uint32x4_t lo, uint32x4_t hi) {
  std::array<std::uint32_t, 8> out{};
  vst1q_u32(out.data(), lo);
  vst1q_u32(out.data() + 4, hi);
  return out;
}

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{vrndmq_f32(v.lo), vrndmq_f32(v.hi)});
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeI32x8(vreinterpretq_s32_f32(v.lo), vreinterpretq_s32_f32(v.hi));
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeU32x8(vreinterpretq_u32_f32(v.lo), vreinterpretq_u32_f32(v.hi));
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vceqq_f32(a.lo, b.lo)),
                              vreinterpretq_f32_u32(vceqq_f32(a.hi, b.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vmvnq_u32(vceqq_f32(a.lo, b.lo))),
                              vreinterpretq_f32_u32(vmvnq_u32(vceqq_f32(a.hi, b.hi)))});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vcgeq_f32(a.lo, b.lo)),
                              vreinterpretq_f32_u32(vcgeq_f32(a.hi, b.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vcgtq_f32(a.lo, b.lo)),
                              vreinterpretq_f32_u32(vcgtq_f32(a.hi, b.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vcgeq_f32(b.lo, a.lo)),
                              vreinterpretq_f32_u32(vcgeq_f32(b.hi, a.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vcgtq_f32(b.lo, a.lo)),
                              vreinterpretq_f32_u32(vcgtq_f32(b.hi, a.hi))});
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  const auto m = loadF32x8(mask);
  const auto onTrue = loadF32x8(t);
  const auto onFalse = loadF32x8(f);
  return storeF32x8(F32x8Neon{
      vreinterpretq_f32_u32(vbslq_u32(vreinterpretq_u32_f32(m.lo), vreinterpretq_u32_f32(onTrue.lo),
                                      vreinterpretq_u32_f32(onFalse.lo))),
      vreinterpretq_f32_u32(vbslq_u32(vreinterpretq_u32_f32(m.hi), vreinterpretq_u32_f32(onTrue.hi),
                                      vreinterpretq_u32_f32(onFalse.hi)))});
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{vabsq_f32(v.lo), vabsq_f32(v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{vsqrtq_f32(v.lo), vsqrtq_f32(v.hi)});
}

[[nodiscard]] inline float32x4_t recipFast(float32x4_t x) {
  float32x4_t approx = vrecpeq_f32(x);
  approx = vmulq_f32(vrecpsq_f32(x, approx), approx);
  return approx;
}

[[nodiscard]] inline float32x4_t recipSqrt(float32x4_t x) {
  float32x4_t approx = vrsqrteq_f32(x);
  approx = vmulq_f32(vrsqrtsq_f32(x, vmulq_f32(approx, approx)), approx);
  return approx;
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{recipFast(v.lo), recipFast(v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{recipSqrt(v.lo), recipSqrt(v.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vmaxq_f32(a.lo, b.lo), vmaxq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vminq_f32(a.lo, b.lo), vminq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{vrndnq_f32(v.lo), vrndnq_f32(v.hi)});
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeI32x8(vcvtnq_s32_f32(v.lo), vcvtnq_s32_f32(v.hi));
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeI32x8(vcvtq_s32_f32(v.lo), vcvtq_s32_f32(v.hi));
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vaddq_f32(a.lo, b.lo), vaddq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vsubq_f32(a.lo, b.lo), vsubq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vmulq_f32(a.lo, b.lo), vmulq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{vdivq_f32(a.lo, b.lo), vdivq_f32(a.hi, b.hi)});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{
      vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(a.lo), vreinterpretq_u32_f32(b.lo))),
      vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(a.hi), vreinterpretq_u32_f32(b.hi)))});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{
      vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a.lo), vreinterpretq_u32_f32(b.lo))),
      vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a.hi), vreinterpretq_u32_f32(b.hi)))});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  const auto a = loadF32x8(lhs);
  const auto b = loadF32x8(rhs);
  return storeF32x8(F32x8Neon{
      vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(a.lo), vreinterpretq_u32_f32(b.lo))),
      vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(a.hi), vreinterpretq_u32_f32(b.hi)))});
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  const auto v = loadF32x8(lanes);
  return storeF32x8(F32x8Neon{vreinterpretq_f32_u32(vmvnq_u32(vreinterpretq_u32_f32(v.lo))),
                              vreinterpretq_f32_u32(vmvnq_u32(vreinterpretq_u32_f32(v.hi)))});
}

#else

[[nodiscard]] inline std::array<float, 8> f32x8Floor(const std::array<float, 8>& lanes) {
  return scalar::f32x8Floor(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8ToI32Bitcast(
    const std::array<float, 8>& lanes) {
  return scalar::f32x8ToI32Bitcast(lanes);
}

[[nodiscard]] inline std::array<std::uint32_t, 8> f32x8ToU32Bitcast(
    const std::array<float, 8>& lanes) {
  return scalar::f32x8ToU32Bitcast(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpEq(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpEq(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpNe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpNe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpGe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpGt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpGt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLe(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpLe(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8CmpLt(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8CmpLt(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Blend(const std::array<float, 8>& mask,
                                                     const std::array<float, 8>& t,
                                                     const std::array<float, 8>& f) {
  return scalar::f32x8Blend(mask, t, f);
}

[[nodiscard]] inline std::array<float, 8> f32x8Abs(const std::array<float, 8>& lanes) {
  return scalar::f32x8Abs(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Sqrt(const std::array<float, 8>& lanes) {
  return scalar::f32x8Sqrt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipFast(const std::array<float, 8>& lanes) {
  return scalar::f32x8RecipFast(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8RecipSqrt(const std::array<float, 8>& lanes) {
  return scalar::f32x8RecipSqrt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Max(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Max(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Min(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Min(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Round(const std::array<float, 8>& lanes) {
  return scalar::f32x8Round(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8RoundInt(const std::array<float, 8>& lanes) {
  return scalar::f32x8RoundInt(lanes);
}

[[nodiscard]] inline std::array<std::int32_t, 8> f32x8TruncInt(const std::array<float, 8>& lanes) {
  return scalar::f32x8TruncInt(lanes);
}

[[nodiscard]] inline std::array<float, 8> f32x8Add(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Add(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Sub(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Sub(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Mul(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Mul(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8Div(const std::array<float, 8>& lhs,
                                                   const std::array<float, 8>& rhs) {
  return scalar::f32x8Div(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitAnd(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return scalar::f32x8BitAnd(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitOr(const std::array<float, 8>& lhs,
                                                     const std::array<float, 8>& rhs) {
  return scalar::f32x8BitOr(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitXor(const std::array<float, 8>& lhs,
                                                      const std::array<float, 8>& rhs) {
  return scalar::f32x8BitXor(lhs, rhs);
}

[[nodiscard]] inline std::array<float, 8> f32x8BitNot(const std::array<float, 8>& lanes) {
  return scalar::f32x8BitNot(lanes);
}

#endif

}  // namespace tiny_skia::wide::backend::aarch64_neon
