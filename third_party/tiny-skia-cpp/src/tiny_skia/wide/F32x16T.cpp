#include "tiny_skia/wide/F32x16T.h"

#include "tiny_skia/wide/U16x16T.h"
#include "tiny_skia/wide/backend/ScalarF32x16T.h"

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace tiny_skia::wide {

F32x16T F32x16T::abs() const { return F32x16T(lo_.abs(), hi_.abs()); }

F32x16T F32x16T::cmpGt(const F32x16T& rhs) const {
  return F32x16T(lo_.cmpGt(rhs.lo_), hi_.cmpGt(rhs.hi_));
}

F32x16T F32x16T::blend(const F32x16T& t, const F32x16T& f) const {
  return F32x16T(lo_.blend(t.lo_, f.lo_), hi_.blend(t.hi_, f.hi_));
}

F32x16T F32x16T::normalize() const { return F32x16T(lo_.normalize(), hi_.normalize()); }

F32x16T F32x16T::floor() const {
  // Skia computes floor via round and mask blend to avoid libm edge semantics.
  const F32x16T roundtrip = round();
  return roundtrip - roundtrip.cmpGt(*this).blend(F32x16T::splat(1.0f), F32x16T::splat(0.0f));
}

F32x16T F32x16T::sqrt() const { return F32x16T(lo_.sqrt(), hi_.sqrt()); }

F32x16T F32x16T::round() const { return F32x16T(lo_.round(), hi_.round()); }

// This method is too heavy and shouldn't be inlined.
void F32x16T::saveToU16x16(U16x16T& dst) const {
  // Do not use roundInt, because it involves rounding,
  // and Skia casts without it.
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  // Vectorized float→uint16 conversion: vcvtq_u32_f32 (truncate) + vmovn_u32 (narrow).
  const auto loLanes = lo_.lanes();
  const auto hiLanes = hi_.lanes();

  const uint32x4_t u0 = vcvtq_u32_f32(vld1q_f32(&loLanes[0]));
  const uint32x4_t u1 = vcvtq_u32_f32(vld1q_f32(&loLanes[4]));
  const uint32x4_t u2 = vcvtq_u32_f32(vld1q_f32(&hiLanes[0]));
  const uint32x4_t u3 = vcvtq_u32_f32(vld1q_f32(&hiLanes[4]));

  dst = U16x16T(vcombine_u16(vmovn_u32(u0), vmovn_u32(u1)),
                vcombine_u16(vmovn_u32(u2), vmovn_u32(u3)));
#else
  backend::scalar::f32x16SaveToU16x16(*this, dst);
#endif
}

F32x16T F32x16T::operator+(const F32x16T& rhs) const {
  return F32x16T(lo_ + rhs.lo_, hi_ + rhs.hi_);
}

F32x16T F32x16T::operator-(const F32x16T& rhs) const {
  return F32x16T(lo_ - rhs.lo_, hi_ - rhs.hi_);
}

F32x16T F32x16T::operator*(const F32x16T& rhs) const {
  return F32x16T(lo_ * rhs.lo_, hi_ * rhs.hi_);
}

}  // namespace tiny_skia::wide
