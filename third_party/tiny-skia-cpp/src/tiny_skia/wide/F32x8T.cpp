#include "tiny_skia/wide/F32x8T.h"

#include <bit>

#include "tiny_skia/wide/I32x8T.h"
#include "tiny_skia/wide/U32x8T.h"
#include "tiny_skia/wide/backend/Aarch64NeonF32x8T.h"
#include "tiny_skia/wide/backend/ScalarF32x8T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaF32x8T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useX86Avx2FmaF32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useAarch64NeonF32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

}  // namespace

float F32x8T::cmpMask(bool predicate) { return backend::scalar::f32x8CmpMask(predicate); }

F32x8T F32x8T::floor() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Floor(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Floor(lanes_));
  }

  return F32x8T(backend::scalar::f32x8Floor(lanes_));
}

F32x8T F32x8T::fract() const { return *this - floor(); }

F32x8T F32x8T::normalize() const { return max(F32x8T::splat(0.0f)).min(F32x8T::splat(1.0f)); }

I32x8T F32x8T::toI32x8Bitcast() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return I32x8T(backend::x86_avx2_fma::f32x8ToI32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return I32x8T(backend::aarch64_neon::f32x8ToI32Bitcast(lanes_));
  }

  return I32x8T(backend::scalar::f32x8ToI32Bitcast(lanes_));
}

U32x8T F32x8T::toU32x8Bitcast() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return U32x8T(backend::x86_avx2_fma::f32x8ToU32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return U32x8T(backend::aarch64_neon::f32x8ToU32Bitcast(lanes_));
  }

  return U32x8T(backend::scalar::f32x8ToU32Bitcast(lanes_));
}

F32x8T F32x8T::cmpEq(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpEq(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpEq(lanes_, rhs.lanes_));
}

F32x8T F32x8T::cmpNe(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpNe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpNe(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpNe(lanes_, rhs.lanes_));
}

F32x8T F32x8T::cmpGe(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpGe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpGe(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpGe(lanes_, rhs.lanes_));
}

F32x8T F32x8T::cmpGt(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpGt(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpGt(lanes_, rhs.lanes_));
}

F32x8T F32x8T::cmpLe(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpLe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpLe(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpLe(lanes_, rhs.lanes_));
}

F32x8T F32x8T::cmpLt(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8CmpLt(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8CmpLt(lanes_, rhs.lanes_));
}

F32x8T F32x8T::blend(const F32x8T& t, const F32x8T& f) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Blend(lanes_, t.lanes_, f.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Blend(lanes_, t.lanes_, f.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Blend(lanes_, t.lanes_, f.lanes_));
}

F32x8T F32x8T::abs() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Abs(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Abs(lanes_));
  }

  return F32x8T(backend::scalar::f32x8Abs(lanes_));
}

F32x8T F32x8T::sqrt() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Sqrt(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Sqrt(lanes_));
  }

  return F32x8T(backend::scalar::f32x8Sqrt(lanes_));
}

F32x8T F32x8T::recipFast() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8RecipFast(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8RecipFast(lanes_));
  }

  return F32x8T(backend::scalar::f32x8RecipFast(lanes_));
}

F32x8T F32x8T::recipSqrt() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8RecipSqrt(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8RecipSqrt(lanes_));
  }

  return F32x8T(backend::scalar::f32x8RecipSqrt(lanes_));
}

F32x8T F32x8T::powf(float exp) const { return F32x8T(backend::scalar::f32x8Powf(lanes_, exp)); }

F32x8T F32x8T::max(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Max(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Max(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Max(lanes_, rhs.lanes_));
}

F32x8T F32x8T::min(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Min(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Min(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Min(lanes_, rhs.lanes_));
}

F32x8T F32x8T::isFinite() const {
  const U32x8T shiftedExpMask = U32x8T::splat(0xFF000000u);
  const U32x8T u = toU32x8Bitcast();
  const U32x8T shiftU = u.shl<1>();
  const U32x8T out = ~(shiftU & shiftedExpMask).cmpEq(shiftedExpMask);
  return out.toF32x8Bitcast();
}

F32x8T F32x8T::round() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Round(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Round(lanes_));
  }

  return F32x8T(backend::scalar::f32x8Round(lanes_));
}

I32x8T F32x8T::roundInt() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return I32x8T(backend::x86_avx2_fma::f32x8RoundInt(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return I32x8T(backend::aarch64_neon::f32x8RoundInt(lanes_));
  }

  return I32x8T(backend::scalar::f32x8RoundInt(lanes_));
}

I32x8T F32x8T::truncInt() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return I32x8T(backend::x86_avx2_fma::f32x8TruncInt(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return I32x8T(backend::aarch64_neon::f32x8TruncInt(lanes_));
  }

  return I32x8T(backend::scalar::f32x8TruncInt(lanes_));
}

F32x8T F32x8T::operator+(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Add(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Add(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Add(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator-(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Sub(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Sub(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Sub(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator*(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Mul(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Mul(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Mul(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator/(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8Div(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8Div(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8Div(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator&(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8BitAnd(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8BitAnd(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8BitAnd(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator|(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8BitOr(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8BitOr(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8BitOr(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator^(const F32x8T& rhs) const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8BitXor(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8BitXor(lanes_, rhs.lanes_));
  }

  return F32x8T(backend::scalar::f32x8BitXor(lanes_, rhs.lanes_));
}

F32x8T F32x8T::operator-() const { return F32x8T::splat(0.0f) - *this; }

F32x8T F32x8T::operator~() const {
  if constexpr (useX86Avx2FmaF32x8()) {
    return F32x8T(backend::x86_avx2_fma::f32x8BitNot(lanes_));
  }
  if constexpr (useAarch64NeonF32x8()) {
    return F32x8T(backend::aarch64_neon::f32x8BitNot(lanes_));
  }

  return F32x8T(backend::scalar::f32x8BitNot(lanes_));
}

bool F32x8T::operator==(const F32x8T& rhs) const { return lanes_ == rhs.lanes_; }

F32x8T& F32x8T::operator+=(const F32x8T& rhs) {
  *this = *this + rhs;
  return *this;
}

}  // namespace tiny_skia::wide
