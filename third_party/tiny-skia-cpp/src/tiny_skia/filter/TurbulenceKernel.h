#pragma once

/// @file TurbulenceKernel.h
/// @brief The four-channel Perlin lattice blend behind the float feTurbulence path.
///
/// Kept separate from `Turbulence.cpp` so the vectorized blend can be
/// cross-validated against a scalar reference directly, without reconstructing
/// the generator's permutation and gradient tables.

// Selects the ISA branch below and pulls in the matching intrinsics header.
#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

/// The four lattice corners' gradient components in structure-of-arrays form:
/// each pointer addresses one corner's four channel values (R, G, B, A), so a
/// single 4-lane load covers every channel of that corner.
///
/// Corners are named by their lattice cell coordinates, so `01` is x = 0, y = 1.
struct TurbulenceCorners {
  const float* gradientX00;
  const float* gradientY00;
  const float* gradientX10;
  const float* gradientY10;
  const float* gradientX01;
  const float* gradientY01;
  const float* gradientX11;
  const float* gradientY11;
};

/// The sample's position inside the lattice cell, and the s-curve interpolation
/// weights derived from it.
struct TurbulenceWeights {
  float rx0;  ///< Fractional x offset from the x = 0 lattice line.
  float rx1;  ///< rx0 - 1, the offset from the x = 1 lattice line.
  float ry0;  ///< Fractional y offset from the y = 0 lattice line.
  float ry1;  ///< ry0 - 1, the offset from the y = 1 lattice line.
  float sx;   ///< s-curve weight for the interpolation along x.
  float sy;   ///< s-curve weight for the interpolation along y.
};

/// Evaluates one Perlin lattice cell for all four channels at once, writing
/// R, G, B, A to `out`.
///
/// Per channel this is the usual bilinear blend of four gradient dot products:
///
///     u      = gradientX00 * rx0 + gradientY00 * ry0
///     v      = gradientX10 * rx1 + gradientY10 * ry0
///     top    = u + sx * (v - u)
///     u      = gradientX01 * rx0 + gradientY01 * ry1
///     v      = gradientX11 * rx1 + gradientY11 * ry1
///     bottom = u + sx * (v - u)
///     out    = top + sy * (bottom - top)
///
/// The wasm128, SSE2, and scalar branches issue exactly those multiplies,
/// adds, and subtracts in exactly that order, so every lane agrees bit for bit.
/// The NEON branch fuses each multiply and add into one rounding step, which
/// can differ by one unit in the last place; that predates the other branches
/// and changing it would move rendered output on ARM.
inline void turbulenceBlend4(const TurbulenceCorners& corners, const TurbulenceWeights& weights,
                             float out[4]) {
#if defined(TINY_SKIA_SIMD_NEON)
  const float32x4_t gx00 = vld1q_f32(corners.gradientX00);
  const float32x4_t gy00 = vld1q_f32(corners.gradientY00);
  const float32x4_t gx10 = vld1q_f32(corners.gradientX10);
  const float32x4_t gy10 = vld1q_f32(corners.gradientY10);
  const float32x4_t gx01 = vld1q_f32(corners.gradientX01);
  const float32x4_t gy01 = vld1q_f32(corners.gradientY01);
  const float32x4_t gx11 = vld1q_f32(corners.gradientX11);
  const float32x4_t gy11 = vld1q_f32(corners.gradientY11);

  // 4 dot products: dot = gx * rx + gy * ry
  const float32x4_t d00 = vfmaq_n_f32(vmulq_n_f32(gx00, weights.rx0), gy00, weights.ry0);
  const float32x4_t d10 = vfmaq_n_f32(vmulq_n_f32(gx10, weights.rx1), gy10, weights.ry0);
  const float32x4_t d01 = vfmaq_n_f32(vmulq_n_f32(gx01, weights.rx0), gy01, weights.ry1);
  const float32x4_t d11 = vfmaq_n_f32(vmulq_n_f32(gx11, weights.rx1), gy11, weights.ry1);

  // Bilinear interpolation: lerp along x, then along y.
  const float32x4_t sx = vdupq_n_f32(weights.sx);
  const float32x4_t top = vfmaq_f32(d00, sx, vsubq_f32(d10, d00));
  const float32x4_t bottom = vfmaq_f32(d01, sx, vsubq_f32(d11, d01));

  const float32x4_t sy = vdupq_n_f32(weights.sy);
  vst1q_f32(out, vfmaq_f32(top, sy, vsubq_f32(bottom, top)));

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  const v128_t rx0 = wasm_f32x4_splat(weights.rx0);
  const v128_t rx1 = wasm_f32x4_splat(weights.rx1);
  const v128_t ry0 = wasm_f32x4_splat(weights.ry0);
  const v128_t ry1 = wasm_f32x4_splat(weights.ry1);

  // 4 dot products: dot = gx * rx + gy * ry. wasm128 has no fused
  // multiply-add outside relaxed SIMD, so each product rounds before the add,
  // exactly like the scalar branch.
  const v128_t d00 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(corners.gradientX00), rx0),
                                    wasm_f32x4_mul(wasm_v128_load(corners.gradientY00), ry0));
  const v128_t d10 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(corners.gradientX10), rx1),
                                    wasm_f32x4_mul(wasm_v128_load(corners.gradientY10), ry0));
  const v128_t d01 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(corners.gradientX01), rx0),
                                    wasm_f32x4_mul(wasm_v128_load(corners.gradientY01), ry1));
  const v128_t d11 = wasm_f32x4_add(wasm_f32x4_mul(wasm_v128_load(corners.gradientX11), rx1),
                                    wasm_f32x4_mul(wasm_v128_load(corners.gradientY11), ry1));

  // Bilinear interpolation: lerp along x, then along y.
  const v128_t sx = wasm_f32x4_splat(weights.sx);
  const v128_t top = wasm_f32x4_add(d00, wasm_f32x4_mul(sx, wasm_f32x4_sub(d10, d00)));
  const v128_t bottom = wasm_f32x4_add(d01, wasm_f32x4_mul(sx, wasm_f32x4_sub(d11, d01)));

  const v128_t sy = wasm_f32x4_splat(weights.sy);
  wasm_v128_store(out, wasm_f32x4_add(top, wasm_f32x4_mul(sy, wasm_f32x4_sub(bottom, top))));

#elif defined(TINY_SKIA_SIMD_SSE2)
  const __m128 rx0 = _mm_set1_ps(weights.rx0);
  const __m128 rx1 = _mm_set1_ps(weights.rx1);
  const __m128 ry0 = _mm_set1_ps(weights.ry0);
  const __m128 ry1 = _mm_set1_ps(weights.ry1);

  // 4 dot products: dot = gx * rx + gy * ry. The library builds with
  // -ffp-contract=off, so the multiply and add stay separately rounded, exactly
  // like the scalar branch.
  const __m128 d00 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(corners.gradientX00), rx0),
                                _mm_mul_ps(_mm_loadu_ps(corners.gradientY00), ry0));
  const __m128 d10 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(corners.gradientX10), rx1),
                                _mm_mul_ps(_mm_loadu_ps(corners.gradientY10), ry0));
  const __m128 d01 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(corners.gradientX01), rx0),
                                _mm_mul_ps(_mm_loadu_ps(corners.gradientY01), ry1));
  const __m128 d11 = _mm_add_ps(_mm_mul_ps(_mm_loadu_ps(corners.gradientX11), rx1),
                                _mm_mul_ps(_mm_loadu_ps(corners.gradientY11), ry1));

  // Bilinear interpolation: lerp along x, then along y.
  const __m128 sx = _mm_set1_ps(weights.sx);
  const __m128 top = _mm_add_ps(d00, _mm_mul_ps(sx, _mm_sub_ps(d10, d00)));
  const __m128 bottom = _mm_add_ps(d01, _mm_mul_ps(sx, _mm_sub_ps(d11, d01)));

  const __m128 sy = _mm_set1_ps(weights.sy);
  _mm_storeu_ps(out, _mm_add_ps(top, _mm_mul_ps(sy, _mm_sub_ps(bottom, top))));

#else
  for (int ch = 0; ch < 4; ++ch) {
    float u = corners.gradientX00[ch] * weights.rx0 + corners.gradientY00[ch] * weights.ry0;
    float v = corners.gradientX10[ch] * weights.rx1 + corners.gradientY10[ch] * weights.ry0;
    const float top = u + weights.sx * (v - u);

    u = corners.gradientX01[ch] * weights.rx0 + corners.gradientY01[ch] * weights.ry1;
    v = corners.gradientX11[ch] * weights.rx1 + corners.gradientY11[ch] * weights.ry1;
    const float bottom = u + weights.sx * (v - u);

    out[ch] = top + weights.sy * (bottom - top);
  }
#endif
}

}  // namespace tiny_skia::filter
