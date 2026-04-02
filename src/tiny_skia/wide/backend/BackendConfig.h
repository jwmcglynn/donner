#pragma once

namespace tiny_skia::wide::backend {

enum class SimdBackend {
  kScalar,
  kX86Avx2Fma,
  kAarch64Neon,
  kWasmSimd128,
  kWasmRelaxedSimd,
};

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) == defined(TINYSKIA_CFG_IF_SIMD_SCALAR)
#error "Exactly one of TINYSKIA_CFG_IF_SIMD_NATIVE or TINYSKIA_CFG_IF_SIMD_SCALAR must be set."
#endif

[[nodiscard]] constexpr SimdBackend detectNativeBackend() {
#if defined(__wasm__) && defined(__wasm_relaxed_simd__)
  return SimdBackend::kWasmRelaxedSimd;
#elif defined(__wasm__) && defined(__wasm_simd128__)
  return SimdBackend::kWasmSimd128;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__AVX2__) && defined(__FMA__)
  return SimdBackend::kX86Avx2Fma;
#elif defined(__aarch64__) && defined(__ARM_NEON)
  return SimdBackend::kAarch64Neon;
#else
  return SimdBackend::kScalar;
#endif
}

[[nodiscard]] constexpr SimdBackend selectedBackend() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE)
  return detectNativeBackend();
#else
  return SimdBackend::kScalar;
#endif
}

[[nodiscard]] constexpr const char* backendName(SimdBackend backend) {
  switch (backend) {
    case SimdBackend::kScalar:
      return "scalar";
    case SimdBackend::kX86Avx2Fma:
      return "x86_avx2_fma";
    case SimdBackend::kAarch64Neon:
      return "aarch64_neon";
    case SimdBackend::kWasmSimd128:
      return "wasm_simd128";
    case SimdBackend::kWasmRelaxedSimd:
      return "wasm_relaxed_simd";
  }

  return "unknown";
}

}  // namespace tiny_skia::wide::backend
