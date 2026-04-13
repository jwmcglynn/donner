#include "tiny_skia/wide/I32x4T.h"

#include <array>
#include <cstdint>

#include "tiny_skia/wide/F32x4T.h"
#include "tiny_skia/wide/backend/Aarch64NeonI32x4T.h"
#include "tiny_skia/wide/backend/ScalarI32x4T.h"
#include "tiny_skia/wide/backend/WasmSimd128I32x4T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaI32x4T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useAarch64NeonI32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useX86Avx2FmaI32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useWasmSimd128I32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__wasm_simd128__)
  return true;
#else
  return false;
#endif
}

}  // namespace

I32x4T I32x4T::blend(const I32x4T& t, const I32x4T& f) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4Blend(lanes_, t.lanes_, f.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4Blend(lanes_, t.lanes_, f.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4Blend(lanes_, t.lanes_, f.lanes_));
  }

  return I32x4T(backend::scalar::i32x4Blend(lanes_, t.lanes_, f.lanes_));
}

I32x4T I32x4T::cmpEq(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4CmpEq(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4CmpEq(lanes_, rhs.lanes_));
}

I32x4T I32x4T::cmpGt(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4CmpGt(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4CmpGt(lanes_, rhs.lanes_));
}

I32x4T I32x4T::cmpLt(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4CmpLt(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4CmpLt(lanes_, rhs.lanes_));
}

F32x4T I32x4T::toF32x4() const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return F32x4T(backend::x86_avx2_fma::i32x4ToF32(lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return F32x4T(backend::aarch64_neon::i32x4ToF32(lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return F32x4T(backend::wasm_simd128::i32x4ToF32(lanes_));
  }

  return F32x4T(backend::scalar::i32x4ToF32(lanes_));
}

F32x4T I32x4T::toF32x4Bitcast() const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return F32x4T(backend::x86_avx2_fma::i32x4ToF32Bitcast(lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return F32x4T(backend::wasm_simd128::i32x4ToF32Bitcast(lanes_));
  }
  return F32x4T(backend::scalar::i32x4ToF32Bitcast(lanes_));
}

I32x4T I32x4T::operator+(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4Add(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4Add(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4Add(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4Add(lanes_, rhs.lanes_));
}

I32x4T I32x4T::operator*(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4Mul(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4Mul(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4Mul(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4Mul(lanes_, rhs.lanes_));
}

I32x4T I32x4T::operator&(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4And(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4And(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4And(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4And(lanes_, rhs.lanes_));
}

I32x4T I32x4T::operator|(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4Or(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4Or(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4Or(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4Or(lanes_, rhs.lanes_));
}

I32x4T I32x4T::operator^(const I32x4T& rhs) const {
  if constexpr (useX86Avx2FmaI32x4()) {
    return I32x4T(backend::x86_avx2_fma::i32x4Xor(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x4()) {
    return I32x4T(backend::aarch64_neon::i32x4Xor(lanes_, rhs.lanes_));
  }
  if constexpr (useWasmSimd128I32x4()) {
    return I32x4T(backend::wasm_simd128::i32x4Xor(lanes_, rhs.lanes_));
  }

  return I32x4T(backend::scalar::i32x4Xor(lanes_, rhs.lanes_));
}

}  // namespace tiny_skia::wide
