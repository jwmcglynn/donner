#pragma once

/// @file SimdVec.h
/// @brief Register-resident SIMD vector types for filter hot loops.
///
/// Unlike the wide/ types which use std::array storage with load/store on every operation,
/// these types hold native SIMD registers directly for use in tight inner loops.

#include <cstdint>

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define TINY_SKIA_SIMD_NEON 1
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define TINY_SKIA_SIMD_WASM_SIMD128 1
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__SSE2__)
#include <emmintrin.h>
#define TINY_SKIA_SIMD_SSE2 1
#endif

namespace tiny_skia::filter {

// ---------------------------------------------------------------------------
// ScaledDivider — multiplicative inverse to avoid integer division.
//
// Precomputes factor = round((1.0 / divisor) * 2^32).
// Then: result = (uint64_t(num) * factor) >> 32
// This is Skia's ScaledDividerU32 technique.
// ---------------------------------------------------------------------------

struct ScaledDivider {
  std::uint32_t factor;
  std::uint32_t half;  // divisor/2 for rounding

  /// Build a divider for `divisor`, which must be at least 2: the reciprocal
  /// of 1 is 2^32, which does not fit in the 32-bit factor. Callers with a
  /// possible divisor of 1 have nothing to divide and should skip the pass.
  explicit ScaledDivider(std::uint32_t divisor)
      : factor(static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(1) << 32) / divisor +
            ((static_cast<std::uint64_t>(1) << 32) % divisor != 0 ? 1 : 0))),
        half(divisor / 2) {}

  /// Divide num by the precomputed divisor with rounding.
  [[nodiscard]] std::uint32_t divide(std::uint32_t num) const {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(num + half) * factor) >> 32);
  }
};

// ---------------------------------------------------------------------------
// Vec4u32 — 4 × uint32 register-resident vector.
// Used for accumulating RGBA channels in the uint8 box blur path.
// ---------------------------------------------------------------------------

struct Vec4u32 {
#if defined(TINY_SKIA_SIMD_NEON)
  uint32x4_t v;

  Vec4u32() : v(vdupq_n_u32(0)) {}
  explicit Vec4u32(uint32x4_t val) : v(val) {}
  static Vec4u32 splat(std::uint32_t x) { return Vec4u32(vdupq_n_u32(x)); }

  /// Load 4 bytes from ptr, zero-extend each to uint32.
  static Vec4u32 loadFromU8(const std::uint8_t* ptr) {
    // Load 4 bytes into the low lane of a uint32x2, then reinterpret as
    // uint8x8 (only low 4 bytes used) and widen to u16, then u32.
    // vld1_lane_u32 returns uint32x2_t; keep that type until the explicit
    // vreinterpret so the code is valid under GCC strict NEON typing rules.
    uint32x2_t b32x2 = vld1_lane_u32(reinterpret_cast<const std::uint32_t*>(ptr),
                                     vdup_n_u32(0), 0);
    uint16x8_t b16 = vmovl_u8(vreinterpret_u8_u32(b32x2));
    uint32x4_t b32 = vmovl_u16(vget_low_u16(b16));
    return Vec4u32(b32);
  }

  /// Narrow uint32 lanes to uint8 and store 4 bytes.
  void storeToU8(std::uint8_t* ptr) const {
    // Narrow u32 -> u16 -> u8.
    uint16x4_t n16 = vmovn_u32(v);
    uint8x8_t n8 = vmovn_u16(vcombine_u16(n16, n16));
    // Store low 4 bytes.
    vst1_lane_u32(reinterpret_cast<std::uint32_t*>(ptr),
                  vreinterpret_u32_u8(n8), 0);
  }

  Vec4u32 operator+(Vec4u32 o) const { return Vec4u32(vaddq_u32(v, o.v)); }
  Vec4u32 operator-(Vec4u32 o) const { return Vec4u32(vsubq_u32(v, o.v)); }
  Vec4u32& operator+=(Vec4u32 o) { v = vaddq_u32(v, o.v); return *this; }
  Vec4u32& operator-=(Vec4u32 o) { v = vsubq_u32(v, o.v); return *this; }

  /// ScaledDivider: (v + half) * factor >> 32 for each lane.
  [[nodiscard]] Vec4u32 scaledDivide(const ScaledDivider& d) const {
    uint32x4_t vhalf = vdupq_n_u32(d.half);
    uint32x4_t vfactor = vdupq_n_u32(d.factor);
    uint32x4_t rounded = vaddq_u32(v, vhalf);
    // Multiply high: (a * b) >> 32 for each 32-bit lane.
    // NEON doesn't have a direct mul-high-u32, so we use pairs of vmull.
    uint64x2_t lo = vmull_u32(vget_low_u32(rounded), vget_low_u32(vfactor));
    uint64x2_t hi = vmull_u32(vget_high_u32(rounded), vget_high_u32(vfactor));
    // Extract high 32 bits of each 64-bit result.
    uint32x2_t lo_hi = vshrn_n_u64(lo, 32);
    uint32x2_t hi_hi = vshrn_n_u64(hi, 32);
    return Vec4u32(vcombine_u32(lo_hi, hi_hi));
  }

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  v128_t v;

  Vec4u32() : v(wasm_u32x4_splat(0)) {}
  explicit Vec4u32(v128_t val) : v(val) {}
  static Vec4u32 splat(std::uint32_t x) { return Vec4u32(wasm_u32x4_splat(x)); }

  /// Load 4 bytes from ptr, zero-extend each to uint32.
  static Vec4u32 loadFromU8(const std::uint8_t* ptr) {
    // Load 4 bytes into the low lane with the rest zeroed (an unaligned load
    // is well defined here), then widen u8 -> u16 -> u32.
    v128_t b8 = wasm_v128_load32_zero(ptr);
    v128_t b16 = wasm_u16x8_extend_low_u8x16(b8);
    return Vec4u32(wasm_u32x4_extend_low_u16x8(b16));
  }

  /// Narrow uint32 lanes to uint8 and store 4 bytes.
  void storeToU8(std::uint8_t* ptr) const {
    // wasm128 only offers saturating narrows, so gather the low byte of every
    // 32-bit lane with a byte shuffle instead. That keeps the scalar
    // static_cast<uint8_t> truncation semantics exactly.
    v128_t packed = wasm_i8x16_shuffle(v, v, 0, 4, 8, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    wasm_v128_store32_lane(ptr, packed, 0);
  }

  Vec4u32 operator+(Vec4u32 o) const { return Vec4u32(wasm_i32x4_add(v, o.v)); }
  Vec4u32 operator-(Vec4u32 o) const { return Vec4u32(wasm_i32x4_sub(v, o.v)); }
  Vec4u32& operator+=(Vec4u32 o) { v = wasm_i32x4_add(v, o.v); return *this; }
  Vec4u32& operator-=(Vec4u32 o) { v = wasm_i32x4_sub(v, o.v); return *this; }

  /// ScaledDivider: (v + half) * factor >> 32 for each lane.
  [[nodiscard]] Vec4u32 scaledDivide(const ScaledDivider& d) const {
    v128_t rounded = wasm_i32x4_add(v, wasm_u32x4_splat(d.half));
    v128_t factor = wasm_u32x4_splat(d.factor);
    // Widening 32x32 -> 64 unsigned multiplies, then keep the high half of
    // each 64-bit product. Lanes are little-endian, so the high half of
    // 64-bit lane n is 32-bit lane 2n+1 and one shuffle collects all four.
    v128_t lo = wasm_u64x2_extmul_low_u32x4(rounded, factor);
    v128_t hi = wasm_u64x2_extmul_high_u32x4(rounded, factor);
    return Vec4u32(wasm_i32x4_shuffle(lo, hi, 1, 3, 5, 7));
  }

#elif defined(TINY_SKIA_SIMD_SSE2)
  __m128i v;

  Vec4u32() : v(_mm_setzero_si128()) {}
  explicit Vec4u32(__m128i val) : v(val) {}
  static Vec4u32 splat(std::uint32_t x) { return Vec4u32(_mm_set1_epi32(static_cast<int>(x))); }

  static Vec4u32 loadFromU8(const std::uint8_t* ptr) {
    // Load 4 bytes, zero-extend to 32-bit.
    __m128i b8 = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(ptr));
    __m128i b16 = _mm_unpacklo_epi8(b8, _mm_setzero_si128());
    __m128i b32 = _mm_unpacklo_epi16(b16, _mm_setzero_si128());
    return Vec4u32(b32);
  }

  void storeToU8(std::uint8_t* ptr) const {
    // Narrow u32 -> u16 -> u8 via packing.
    __m128i packed16 = _mm_packs_epi32(v, v);
    __m128i packed8 = _mm_packus_epi16(packed16, packed16);
    *reinterpret_cast<int*>(ptr) = _mm_cvtsi128_si32(packed8);
  }

  Vec4u32 operator+(Vec4u32 o) const { return Vec4u32(_mm_add_epi32(v, o.v)); }
  Vec4u32 operator-(Vec4u32 o) const { return Vec4u32(_mm_sub_epi32(v, o.v)); }
  Vec4u32& operator+=(Vec4u32 o) { v = _mm_add_epi32(v, o.v); return *this; }
  Vec4u32& operator-=(Vec4u32 o) { v = _mm_sub_epi32(v, o.v); return *this; }

  [[nodiscard]] Vec4u32 scaledDivide(const ScaledDivider& d) const {
    // SSE2 doesn't have 32×32→64 unsigned multiply in a single instruction.
    // We process each lane individually.
    __m128i vhalf = _mm_set1_epi32(static_cast<int>(d.half));
    __m128i rounded = _mm_add_epi32(v, vhalf);
    // Extract each lane, multiply, take high 32 bits.
    alignas(16) std::uint32_t lanes[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), rounded);
    std::uint64_t factor64 = d.factor;
    lanes[0] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(lanes[0]) * factor64) >> 32);
    lanes[1] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(lanes[1]) * factor64) >> 32);
    lanes[2] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(lanes[2]) * factor64) >> 32);
    lanes[3] = static_cast<std::uint32_t>((static_cast<std::uint64_t>(lanes[3]) * factor64) >> 32);
    return Vec4u32(_mm_load_si128(reinterpret_cast<const __m128i*>(lanes)));
  }

#else
  // Scalar fallback.
  std::uint32_t v[4];

  Vec4u32() : v{0, 0, 0, 0} {}
  explicit Vec4u32(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
      : v{a, b, c, d} {}
  static Vec4u32 splat(std::uint32_t x) {
    Vec4u32 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = x;
    return r;
  }

  static Vec4u32 loadFromU8(const std::uint8_t* ptr) {
    Vec4u32 r;
    r.v[0] = ptr[0]; r.v[1] = ptr[1]; r.v[2] = ptr[2]; r.v[3] = ptr[3];
    return r;
  }

  void storeToU8(std::uint8_t* ptr) const {
    ptr[0] = static_cast<std::uint8_t>(v[0]);
    ptr[1] = static_cast<std::uint8_t>(v[1]);
    ptr[2] = static_cast<std::uint8_t>(v[2]);
    ptr[3] = static_cast<std::uint8_t>(v[3]);
  }

  Vec4u32 operator+(Vec4u32 o) const {
    Vec4u32 r;
    r.v[0] = v[0]+o.v[0]; r.v[1] = v[1]+o.v[1]; r.v[2] = v[2]+o.v[2]; r.v[3] = v[3]+o.v[3];
    return r;
  }
  Vec4u32 operator-(Vec4u32 o) const {
    Vec4u32 r;
    r.v[0] = v[0]-o.v[0]; r.v[1] = v[1]-o.v[1]; r.v[2] = v[2]-o.v[2]; r.v[3] = v[3]-o.v[3];
    return r;
  }
  Vec4u32& operator+=(Vec4u32 o) { v[0]+=o.v[0]; v[1]+=o.v[1]; v[2]+=o.v[2]; v[3]+=o.v[3]; return *this; }
  Vec4u32& operator-=(Vec4u32 o) { v[0]-=o.v[0]; v[1]-=o.v[1]; v[2]-=o.v[2]; v[3]-=o.v[3]; return *this; }

  [[nodiscard]] Vec4u32 scaledDivide(const ScaledDivider& d) const {
    Vec4u32 r;
    r.v[0] = d.divide(v[0]); r.v[1] = d.divide(v[1]);
    r.v[2] = d.divide(v[2]); r.v[3] = d.divide(v[3]);
    return r;
  }
#endif
};

// ---------------------------------------------------------------------------
// Vec4f32 — 4 × float register-resident vector.
// Used for float box blur and morphology.
// ---------------------------------------------------------------------------

struct Vec4f32 {
#if defined(TINY_SKIA_SIMD_NEON)
  float32x4_t v;

  Vec4f32() : v(vdupq_n_f32(0.0f)) {}
  explicit Vec4f32(float32x4_t val) : v(val) {}
  static Vec4f32 splat(float x) { return Vec4f32(vdupq_n_f32(x)); }

  static Vec4f32 load(const float* ptr) { return Vec4f32(vld1q_f32(ptr)); }
  void store(float* ptr) const { vst1q_f32(ptr, v); }

  Vec4f32 operator+(Vec4f32 o) const { return Vec4f32(vaddq_f32(v, o.v)); }
  Vec4f32 operator-(Vec4f32 o) const { return Vec4f32(vsubq_f32(v, o.v)); }
  Vec4f32 operator*(Vec4f32 o) const { return Vec4f32(vmulq_f32(v, o.v)); }
  Vec4f32& operator+=(Vec4f32 o) { v = vaddq_f32(v, o.v); return *this; }
  Vec4f32& operator-=(Vec4f32 o) { v = vsubq_f32(v, o.v); return *this; }

  static Vec4f32 min(Vec4f32 a, Vec4f32 b) { return Vec4f32(vminq_f32(a.v, b.v)); }
  static Vec4f32 max(Vec4f32 a, Vec4f32 b) { return Vec4f32(vmaxq_f32(a.v, b.v)); }

  Vec4f32 clamp01() const {
    return Vec4f32(vminq_f32(vmaxq_f32(v, vdupq_n_f32(0.0f)), vdupq_n_f32(1.0f)));
  }

  /// Fused multiply-add: a * b + c.
  static Vec4f32 fmadd(Vec4f32 a, Vec4f32 b, Vec4f32 c) {
    return Vec4f32(vfmaq_f32(c.v, a.v, b.v));
  }

  /// Clamp each lane to [0, maxVal].
  Vec4f32 clampMax(Vec4f32 maxVal) const {
    return Vec4f32(vminq_f32(vmaxq_f32(v, vdupq_n_f32(0.0f)), maxVal.v));
  }

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  v128_t v;

  Vec4f32() : v(wasm_f32x4_splat(0.0f)) {}
  explicit Vec4f32(v128_t val) : v(val) {}
  static Vec4f32 splat(float x) { return Vec4f32(wasm_f32x4_splat(x)); }

  static Vec4f32 load(const float* ptr) { return Vec4f32(wasm_v128_load(ptr)); }
  void store(float* ptr) const { wasm_v128_store(ptr, v); }

  Vec4f32 operator+(Vec4f32 o) const { return Vec4f32(wasm_f32x4_add(v, o.v)); }
  Vec4f32 operator-(Vec4f32 o) const { return Vec4f32(wasm_f32x4_sub(v, o.v)); }
  Vec4f32 operator*(Vec4f32 o) const { return Vec4f32(wasm_f32x4_mul(v, o.v)); }
  Vec4f32& operator+=(Vec4f32 o) { v = wasm_f32x4_add(v, o.v); return *this; }
  Vec4f32& operator-=(Vec4f32 o) { v = wasm_f32x4_sub(v, o.v); return *this; }

  // wasm128's f32x4.min/max are IEEE minimum/maximum: they canonicalize NaN
  // and order signed zeros. f32x4.pmin/pmax are plain lane selects defined as
  // pmin(x, y) = y < x ? y : x and pmax(x, y) = x < y ? y : x, so passing the
  // operands as (b, a) reproduces the scalar `a < b ? a : b` and
  // `a > b ? a : b` selections bit for bit, including NaN and signed zeros.
  static Vec4f32 min(Vec4f32 a, Vec4f32 b) { return Vec4f32(wasm_f32x4_pmin(b.v, a.v)); }
  static Vec4f32 max(Vec4f32 a, Vec4f32 b) { return Vec4f32(wasm_f32x4_pmax(b.v, a.v)); }

  // The scalar clamps are a nested `v < lo ? lo : (v > hi ? hi : v)`, not a
  // min/max composition, and the two differ on NaN and on a negative zero
  // input. pmax(v, lo) is `v < lo ? lo : v` and pmin(t, hi) is
  // `hi < t ? hi : t`, so this operand order reproduces the scalar result bit
  // for bit. The NEON and SSE2 branches compose min/max here instead and
  // diverge from scalar for those inputs; that behaviour predates this branch
  // and is left alone.
  Vec4f32 clamp01() const {
    return Vec4f32(
        wasm_f32x4_pmin(wasm_f32x4_pmax(v, wasm_f32x4_splat(0.0f)), wasm_f32x4_splat(1.0f)));
  }

  /// Multiply-add: a * b + c.
  static Vec4f32 fmadd(Vec4f32 a, Vec4f32 b, Vec4f32 c) {
    // wasm128 has no fused multiply-add (that is a relaxed-SIMD instruction),
    // so this is a separate multiply and add. Each lane therefore rounds
    // twice, exactly like the scalar fallback.
    return Vec4f32(wasm_f32x4_add(wasm_f32x4_mul(a.v, b.v), c.v));
  }

  /// Clamp each lane to [0, maxVal].
  Vec4f32 clampMax(Vec4f32 maxVal) const {
    return Vec4f32(wasm_f32x4_pmin(wasm_f32x4_pmax(v, wasm_f32x4_splat(0.0f)), maxVal.v));
  }

#elif defined(TINY_SKIA_SIMD_SSE2)
  __m128 v;

  Vec4f32() : v(_mm_setzero_ps()) {}
  explicit Vec4f32(__m128 val) : v(val) {}
  static Vec4f32 splat(float x) { return Vec4f32(_mm_set1_ps(x)); }

  static Vec4f32 load(const float* ptr) { return Vec4f32(_mm_loadu_ps(ptr)); }
  void store(float* ptr) const { _mm_storeu_ps(ptr, v); }

  Vec4f32 operator+(Vec4f32 o) const { return Vec4f32(_mm_add_ps(v, o.v)); }
  Vec4f32 operator-(Vec4f32 o) const { return Vec4f32(_mm_sub_ps(v, o.v)); }
  Vec4f32 operator*(Vec4f32 o) const { return Vec4f32(_mm_mul_ps(v, o.v)); }
  Vec4f32& operator+=(Vec4f32 o) { v = _mm_add_ps(v, o.v); return *this; }
  Vec4f32& operator-=(Vec4f32 o) { v = _mm_sub_ps(v, o.v); return *this; }

  static Vec4f32 min(Vec4f32 a, Vec4f32 b) { return Vec4f32(_mm_min_ps(a.v, b.v)); }
  static Vec4f32 max(Vec4f32 a, Vec4f32 b) { return Vec4f32(_mm_max_ps(a.v, b.v)); }

  Vec4f32 clamp01() const {
    return Vec4f32(_mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(1.0f)));
  }

  static Vec4f32 fmadd(Vec4f32 a, Vec4f32 b, Vec4f32 c) {
    return Vec4f32(_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v));
  }

  Vec4f32 clampMax(Vec4f32 maxVal) const {
    return Vec4f32(_mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), maxVal.v));
  }

#else
  float v[4];

  Vec4f32() : v{0, 0, 0, 0} {}
  explicit Vec4f32(float a, float b, float c, float d) : v{a, b, c, d} {}
  static Vec4f32 splat(float x) { Vec4f32 r; r.v[0]=r.v[1]=r.v[2]=r.v[3]=x; return r; }

  static Vec4f32 load(const float* ptr) {
    Vec4f32 r; r.v[0]=ptr[0]; r.v[1]=ptr[1]; r.v[2]=ptr[2]; r.v[3]=ptr[3]; return r;
  }
  void store(float* ptr) const { ptr[0]=v[0]; ptr[1]=v[1]; ptr[2]=v[2]; ptr[3]=v[3]; }

  Vec4f32 operator+(Vec4f32 o) const { Vec4f32 r; r.v[0]=v[0]+o.v[0]; r.v[1]=v[1]+o.v[1]; r.v[2]=v[2]+o.v[2]; r.v[3]=v[3]+o.v[3]; return r; }
  Vec4f32 operator-(Vec4f32 o) const { Vec4f32 r; r.v[0]=v[0]-o.v[0]; r.v[1]=v[1]-o.v[1]; r.v[2]=v[2]-o.v[2]; r.v[3]=v[3]-o.v[3]; return r; }
  Vec4f32 operator*(Vec4f32 o) const { Vec4f32 r; r.v[0]=v[0]*o.v[0]; r.v[1]=v[1]*o.v[1]; r.v[2]=v[2]*o.v[2]; r.v[3]=v[3]*o.v[3]; return r; }
  Vec4f32& operator+=(Vec4f32 o) { v[0]+=o.v[0]; v[1]+=o.v[1]; v[2]+=o.v[2]; v[3]+=o.v[3]; return *this; }
  Vec4f32& operator-=(Vec4f32 o) { v[0]-=o.v[0]; v[1]-=o.v[1]; v[2]-=o.v[2]; v[3]-=o.v[3]; return *this; }

  static Vec4f32 min(Vec4f32 a, Vec4f32 b) {
    Vec4f32 r;
    r.v[0] = a.v[0]<b.v[0]?a.v[0]:b.v[0]; r.v[1] = a.v[1]<b.v[1]?a.v[1]:b.v[1];
    r.v[2] = a.v[2]<b.v[2]?a.v[2]:b.v[2]; r.v[3] = a.v[3]<b.v[3]?a.v[3]:b.v[3];
    return r;
  }
  static Vec4f32 max(Vec4f32 a, Vec4f32 b) {
    Vec4f32 r;
    r.v[0] = a.v[0]>b.v[0]?a.v[0]:b.v[0]; r.v[1] = a.v[1]>b.v[1]?a.v[1]:b.v[1];
    r.v[2] = a.v[2]>b.v[2]?a.v[2]:b.v[2]; r.v[3] = a.v[3]>b.v[3]?a.v[3]:b.v[3];
    return r;
  }
  Vec4f32 clamp01() const {
    Vec4f32 r;
    for (int i = 0; i < 4; ++i) r.v[i] = v[i] < 0.0f ? 0.0f : (v[i] > 1.0f ? 1.0f : v[i]);
    return r;
  }
  static Vec4f32 fmadd(Vec4f32 a, Vec4f32 b, Vec4f32 c) {
    Vec4f32 r;
    for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i];
    return r;
  }
  Vec4f32 clampMax(Vec4f32 maxVal) const {
    Vec4f32 r;
    for (int i = 0; i < 4; ++i) r.v[i] = v[i] < 0.0f ? 0.0f : (v[i] > maxVal.v[i] ? maxVal.v[i] : v[i]);
    return r;
  }
#endif
};

// ---------------------------------------------------------------------------
// Vec4u8 — 4 × uint8 packed in a single register for morphology min/max.
// ---------------------------------------------------------------------------

struct Vec4u8 {
#if defined(TINY_SKIA_SIMD_NEON)
  // Use a uint8x8_t with only lower 4 bytes active.
  uint8x8_t v;

  Vec4u8() : v(vdup_n_u8(0)) {}
  explicit Vec4u8(uint8x8_t val) : v(val) {}

  static Vec4u8 load(const std::uint8_t* ptr) {
    // vld1_lane_u32 returns uint32x2_t; keep that type until the explicit
    // vreinterpret so the code is valid under GCC strict NEON typing rules.
    uint32x2_t r32 = vld1_lane_u32(reinterpret_cast<const std::uint32_t*>(ptr),
                                   vdup_n_u32(0), 0);
    return Vec4u8(vreinterpret_u8_u32(r32));
  }

  void store(std::uint8_t* ptr) const {
    vst1_lane_u32(reinterpret_cast<std::uint32_t*>(ptr),
                  vreinterpret_u32_u8(v), 0);
  }

  static Vec4u8 min(Vec4u8 a, Vec4u8 b) { return Vec4u8(vmin_u8(a.v, b.v)); }
  static Vec4u8 max(Vec4u8 a, Vec4u8 b) { return Vec4u8(vmax_u8(a.v, b.v)); }

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  // Pack 4 bytes in the low 32 bits of a v128; the upper 12 bytes stay zero.
  v128_t v;

  Vec4u8() : v(wasm_u8x16_splat(0)) {}
  explicit Vec4u8(v128_t val) : v(val) {}

  static Vec4u8 load(const std::uint8_t* ptr) { return Vec4u8(wasm_v128_load32_zero(ptr)); }

  void store(std::uint8_t* ptr) const { wasm_v128_store32_lane(ptr, v, 0); }

  static Vec4u8 min(Vec4u8 a, Vec4u8 b) { return Vec4u8(wasm_u8x16_min(a.v, b.v)); }
  static Vec4u8 max(Vec4u8 a, Vec4u8 b) { return Vec4u8(wasm_u8x16_max(a.v, b.v)); }

#elif defined(TINY_SKIA_SIMD_SSE2)
  // Pack 4 bytes in the low 32 bits of an __m128i.
  __m128i v;

  Vec4u8() : v(_mm_setzero_si128()) {}
  explicit Vec4u8(__m128i val) : v(val) {}

  static Vec4u8 load(const std::uint8_t* ptr) {
    return Vec4u8(_mm_cvtsi32_si128(*reinterpret_cast<const int*>(ptr)));
  }

  void store(std::uint8_t* ptr) const {
    *reinterpret_cast<int*>(ptr) = _mm_cvtsi128_si32(v);
  }

  static Vec4u8 min(Vec4u8 a, Vec4u8 b) { return Vec4u8(_mm_min_epu8(a.v, b.v)); }
  static Vec4u8 max(Vec4u8 a, Vec4u8 b) { return Vec4u8(_mm_max_epu8(a.v, b.v)); }

#else
  std::uint8_t v[4];

  Vec4u8() : v{0, 0, 0, 0} {}

  static Vec4u8 load(const std::uint8_t* ptr) {
    Vec4u8 r; r.v[0]=ptr[0]; r.v[1]=ptr[1]; r.v[2]=ptr[2]; r.v[3]=ptr[3]; return r;
  }
  void store(std::uint8_t* ptr) const {
    ptr[0]=v[0]; ptr[1]=v[1]; ptr[2]=v[2]; ptr[3]=v[3];
  }

  static Vec4u8 min(Vec4u8 a, Vec4u8 b) {
    Vec4u8 r;
    r.v[0] = a.v[0]<b.v[0]?a.v[0]:b.v[0]; r.v[1] = a.v[1]<b.v[1]?a.v[1]:b.v[1];
    r.v[2] = a.v[2]<b.v[2]?a.v[2]:b.v[2]; r.v[3] = a.v[3]<b.v[3]?a.v[3]:b.v[3];
    return r;
  }
  static Vec4u8 max(Vec4u8 a, Vec4u8 b) {
    Vec4u8 r;
    r.v[0] = a.v[0]>b.v[0]?a.v[0]:b.v[0]; r.v[1] = a.v[1]>b.v[1]?a.v[1]:b.v[1];
    r.v[2] = a.v[2]>b.v[2]?a.v[2]:b.v[2]; r.v[3] = a.v[3]>b.v[3]?a.v[3]:b.v[3];
    return r;
  }
#endif
};

}  // namespace tiny_skia::filter
