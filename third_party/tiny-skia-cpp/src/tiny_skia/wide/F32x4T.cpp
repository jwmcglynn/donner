#include "tiny_skia/wide/F32x4T.h"

#include <array>
#include <cstdint>

#include "tiny_skia/wide/I32x4T.h"
#include "tiny_skia/wide/backend/Aarch64NeonF32x4T.h"
#include "tiny_skia/wide/backend/ScalarF32x4T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaF32x4T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useAarch64NeonF32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useX86Avx2FmaF32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

}  // namespace

float F32x4T::cmpMask(bool predicate) { return backend::scalar::f32x4CmpMask(predicate); }

F32x4T F32x4T::abs() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Abs(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Abs(lanes_));
  }

  return F32x4T(backend::scalar::f32x4Abs(lanes_));
}

F32x4T F32x4T::max(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Max(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Max(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Max(lanes_, rhs.lanes_));
}

F32x4T F32x4T::min(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Min(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Min(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Min(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpEq(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpEq(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpEq(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpNe(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpNe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpNe(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpNe(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpGe(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpGe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpGe(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpGe(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpGt(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpGt(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpGt(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpLe(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpLe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpLe(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpLe(lanes_, rhs.lanes_));
}

F32x4T F32x4T::cmpLt(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4CmpLt(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4CmpLt(lanes_, rhs.lanes_));
}

F32x4T F32x4T::blend(const F32x4T& t, const F32x4T& f) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Blend(lanes_, t.lanes_, f.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Blend(lanes_, t.lanes_, f.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Blend(lanes_, t.lanes_, f.lanes_));
}

F32x4T F32x4T::floor() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Floor(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Floor(lanes_));
  }

  return F32x4T(backend::scalar::f32x4Floor(lanes_));
}

F32x4T F32x4T::fract() const { return *this - floor(); }

F32x4T F32x4T::normalize() const { return max(F32x4T::splat(0.0f)).min(F32x4T::splat(1.0f)); }

F32x4T F32x4T::round() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Round(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Round(lanes_));
  }

  return F32x4T(backend::scalar::f32x4Round(lanes_));
}

I32x4T F32x4T::roundInt() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return I32x4T(backend::x86_avx2_fma::f32x4RoundInt(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return I32x4T(backend::aarch64_neon::f32x4RoundInt(lanes_));
  }

  return I32x4T(backend::scalar::f32x4RoundInt(lanes_));
}

I32x4T F32x4T::truncInt() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return I32x4T(backend::x86_avx2_fma::f32x4TruncInt(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return I32x4T(backend::aarch64_neon::f32x4TruncInt(lanes_));
  }

  return I32x4T(backend::scalar::f32x4TruncInt(lanes_));
}

I32x4T F32x4T::toI32x4Bitcast() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return I32x4T(backend::x86_avx2_fma::f32x4ToI32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return I32x4T(backend::aarch64_neon::f32x4ToI32Bitcast(lanes_));
  }

  return I32x4T(backend::scalar::f32x4ToI32Bitcast(lanes_));
}

F32x4T F32x4T::recipFast() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4RecipFast(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4RecipFast(lanes_));
  }

  return F32x4T(backend::scalar::f32x4RecipFast(lanes_));
}

F32x4T F32x4T::recipSqrt() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4RecipSqrt(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4RecipSqrt(lanes_));
  }

  return F32x4T(backend::scalar::f32x4RecipSqrt(lanes_));
}

F32x4T F32x4T::sqrt() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Sqrt(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Sqrt(lanes_));
  }

  return F32x4T(backend::scalar::f32x4Sqrt(lanes_));
}

F32x4T F32x4T::operator+(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Add(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Add(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Add(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator-(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Sub(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Sub(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Sub(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator*(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Mul(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Mul(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Mul(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator/(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4Div(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4Div(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4Div(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator&(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4BitAnd(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4BitAnd(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4BitAnd(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator|(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4BitOr(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4BitOr(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4BitOr(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator^(const F32x4T& rhs) const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4BitXor(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4BitXor(lanes_, rhs.lanes_));
  }

  return F32x4T(backend::scalar::f32x4BitXor(lanes_, rhs.lanes_));
}

F32x4T F32x4T::operator-() const { return F32x4T::splat(0.0f) - *this; }

F32x4T F32x4T::operator~() const {
  if constexpr (useX86Avx2FmaF32x4()) {
    return F32x4T(backend::x86_avx2_fma::f32x4BitNot(lanes_));
  }
  if constexpr (useAarch64NeonF32x4()) {
    return F32x4T(backend::aarch64_neon::f32x4BitNot(lanes_));
  }

  return F32x4T(backend::scalar::f32x4BitNot(lanes_));
}

F32x4T& F32x4T::operator+=(const F32x4T& rhs) {
  *this = *this + rhs;
  return *this;
}

F32x4T& F32x4T::operator*=(const F32x4T& rhs) {
  *this = *this * rhs;
  return *this;
}

bool F32x4T::operator==(const F32x4T& rhs) const { return lanes_ == rhs.lanes_; }

}  // namespace tiny_skia::wide
