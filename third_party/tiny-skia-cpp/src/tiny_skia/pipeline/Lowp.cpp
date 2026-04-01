// Disable FMA contraction to match Rust's lowp pipeline, which uses software
// SIMD wrappers (f32x16/f32x8) that prevent LLVM from fusing multiply-add.
#ifdef __clang__
#pragma clang fp contract(off)
#endif

#include "tiny_skia/pipeline/Lowp.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <span>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/wide/F32x16T.h"
#include "tiny_skia/wide/F32x8T.h"
#include "tiny_skia/wide/U16x16T.h"
#include "tiny_skia/wide/backend/Aarch64NeonU16x16T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaU16x16T.h"

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace tiny_skia::pipeline::lowp {

using tiny_skia::wide::F32x16T;
using tiny_skia::wide::F32x8T;
using tiny_skia::wide::U16x16T;

struct Pipeline {
  U16x16T r{};
  U16x16T g{};
  U16x16T b{};
  U16x16T a{};
  U16x16T uniformR{};
  U16x16T uniformG{};
  U16x16T uniformB{};
  U16x16T uniformA{};
  U16x16T dr{};
  U16x16T dg{};
  U16x16T db{};
  U16x16T da{};

  const std::array<StageFn, tiny_skia::pipeline::kMaxStages>* functions = nullptr;
  std::size_t index = 0;
  std::size_t dx = 0;
  std::size_t dy = 0;
  std::size_t tail = 0;
  bool scanlineDone = false;  ///< Set by fused stages that process entire scanline at once.
  const ScreenIntRect* rect = nullptr;
  const AAMaskCtx* aaMaskCtx = nullptr;
  const MaskCtx* maskCtx = nullptr;
  Context* ctx = nullptr;
  MutableSubPixmapView* pixmapDst = nullptr;

  Pipeline(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& fun,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& tailFun,
           const ScreenIntRect& rectArg, const AAMaskCtx& aaMaskCtxArg,
           const MaskCtx& maskCtxArg, Context& ctxArg, MutableSubPixmapView* pixmapDstArg)
      : uniformR(U16x16T::splat(ctxArg.uniformColor.rgba[0])),
        uniformG(U16x16T::splat(ctxArg.uniformColor.rgba[1])),
        uniformB(U16x16T::splat(ctxArg.uniformColor.rgba[2])),
        uniformA(U16x16T::splat(ctxArg.uniformColor.rgba[3])),
        functions(&fun),
        rect(&rectArg),
        aaMaskCtx(&aaMaskCtxArg),
        maskCtx(&maskCtxArg),
        ctx(&ctxArg),
        pixmapDst(pixmapDstArg) {
    (void)tailFun;
  }

  void nextStage() {
    const auto next = (*functions)[index];
    ++index;
    next(*this);
  }
};

bool fnPtrEq(StageFn a, StageFn b) { return a == b; }

const void* fnPtr(StageFn fn) { return reinterpret_cast<const void*>(fn); }

namespace {

[[nodiscard]] [[maybe_unused]] constexpr bool useAarch64NeonNative() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] [[maybe_unused]] constexpr bool useX86Avx2FmaNative() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

// split(): reinterpret f32x16 (64 bytes) as two u16x16 (32 bytes each)
inline void split(const F32x16T& v, U16x16T& lo, U16x16T& hi) {
  auto loLanes = v.lo().lanes();
  auto hiLanes = v.hi().lanes();
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  // Load float bytes directly into NEON uint16 registers.
  uint16x8_t ll, lh, hl, hh;
  std::memcpy(&ll, &loLanes[0], sizeof(ll));
  std::memcpy(&lh, &loLanes[4], sizeof(lh));
  std::memcpy(&hl, &hiLanes[0], sizeof(hl));
  std::memcpy(&hh, &hiLanes[4], sizeof(hh));
  lo = U16x16T(ll, lh);
  hi = U16x16T(hl, hh);
#else
  std::memcpy(&lo.lanes(), &loLanes, sizeof(lo.lanes()));
  std::memcpy(&hi.lanes(), &hiLanes, sizeof(hi.lanes()));
#endif
}

// join(): reinterpret two u16x16 (32 bytes each) as f32x16 (64 bytes)
inline F32x16T join(const U16x16T& lo, const U16x16T& hi) {
  std::array<float, 8> loF{}, hiF{};
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  // Store NEON registers directly into float arrays.
  uint16x8_t tmp;
  tmp = lo.neonLo(); std::memcpy(&loF[0], &tmp, sizeof(tmp));
  tmp = lo.neonHi(); std::memcpy(&loF[4], &tmp, sizeof(tmp));
  tmp = hi.neonLo(); std::memcpy(&hiF[0], &tmp, sizeof(tmp));
  tmp = hi.neonHi(); std::memcpy(&hiF[4], &tmp, sizeof(tmp));
#else
  std::memcpy(&loF, &lo.lanes(), sizeof(loF));
  std::memcpy(&hiF, &hi.lanes(), sizeof(hiF));
#endif
  return F32x16T(F32x8T(loF), F32x8T(hiF));
}

// div255: exact integer division by 255, equivalent to ((v + 128) * 257) >> 16.
// Uses the two-step formula: v128 = v + 128; result = (v128 + (v128 >> 8)) >> 8.
// This matches Skia's SkMul16ShiftRound with shift=8.
inline U16x16T div255(U16x16T v) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return wide::backend::aarch64_neon::u16x16Div255(v);
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return wide::backend::x86_avx2_fma::u16x16Div255(v);
#else
  const auto v128 = v + U16x16T::splat(128);
  return (v128 + (v128 >> U16x16T::splat(8))) >> U16x16T::splat(8);
#endif
}

inline U16x16T mulDiv255(const U16x16T& lhs, const U16x16T& rhs) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return wide::backend::aarch64_neon::u16x16MulDiv255(lhs, rhs);
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return wide::backend::x86_avx2_fma::u16x16MulDiv255(lhs, rhs);
#else
  return div255(lhs * rhs);
#endif
}

inline U16x16T mulAddDiv255(const U16x16T& lhs0, const U16x16T& rhs0, const U16x16T& lhs1,
                            const U16x16T& rhs1) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return wide::backend::aarch64_neon::u16x16MulAddDiv255(lhs0, rhs0, lhs1, rhs1);
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return wide::backend::x86_avx2_fma::u16x16MulAddDiv255(lhs0, rhs0, lhs1, rhs1);
#else
  return div255(lhs0 * rhs0 + lhs1 * rhs1);
#endif
}

inline U16x16T sourceOverChannel(const U16x16T& source, const U16x16T& dest,
                                 const U16x16T& sourceAlpha) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return wide::backend::aarch64_neon::u16x16SourceOver(source, dest, sourceAlpha);
#elif defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return wide::backend::x86_avx2_fma::u16x16SourceOver(source, dest, sourceAlpha);
#else
  return source + div255(dest * (U16x16T::splat(255) - sourceAlpha));
#endif
}

// fromFloat(f): (f * 255.0 + 0.5) as u16, splatted to all lanes
inline U16x16T fromFloat(float f) {
  return U16x16T::splat(static_cast<std::uint16_t>(f * 255.0f + 0.5f));
}

// roundF32ToU16(): normalize, scale to [0,255], truncate
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)

// NEON-optimized: loads floats once, does clamp→scale→add→convert→narrow
// all in-register, avoiding F32x8T array↔NEON roundtrips.
inline U16x16T neonClampScaleTruncate(const F32x16T& f, bool clamp) {
  const auto loLanes = f.lo().lanes();
  const auto hiLanes = f.hi().lanes();

  const float32x4_t zero = vdupq_n_f32(0.0f);
  const float32x4_t one = vdupq_n_f32(1.0f);
  const float32x4_t scale = vdupq_n_f32(255.0f);
  const float32x4_t half = vdupq_n_f32(0.5f);

  auto convert = [&](const float* ptr) -> float32x4_t {
    float32x4_t v = vld1q_f32(ptr);
    if (clamp) {
      v = vmaxq_f32(v, zero);
      v = vminq_f32(v, one);
    }
    return vaddq_f32(vmulq_f32(v, scale), half);
  };

  const float32x4_t f0 = convert(&loLanes[0]);
  const float32x4_t f1 = convert(&loLanes[4]);
  const float32x4_t f2 = convert(&hiLanes[0]);
  const float32x4_t f3 = convert(&hiLanes[4]);

  return U16x16T(vcombine_u16(vmovn_u32(vcvtq_u32_f32(f0)), vmovn_u32(vcvtq_u32_f32(f1))),
                 vcombine_u16(vmovn_u32(vcvtq_u32_f32(f2)), vmovn_u32(vcvtq_u32_f32(f3))));
}

inline void roundF32ToU16(F32x16T rf, F32x16T gf, F32x16T bf, F32x16T af, U16x16T& r, U16x16T& g,
                          U16x16T& b, U16x16T& a) {
  r = neonClampScaleTruncate(rf, true);
  g = neonClampScaleTruncate(gf, true);
  b = neonClampScaleTruncate(bf, true);
  a = neonClampScaleTruncate(af, false);
}

#else

inline void roundF32ToU16(F32x16T rf, F32x16T gf, F32x16T bf, F32x16T af, U16x16T& r, U16x16T& g,
                          U16x16T& b, U16x16T& a) {
  rf = rf.normalize() * F32x16T::splat(255.0f) + F32x16T::splat(0.5f);
  gf = gf.normalize() * F32x16T::splat(255.0f) + F32x16T::splat(0.5f);
  bf = bf.normalize() * F32x16T::splat(255.0f) + F32x16T::splat(0.5f);
  af = af * F32x16T::splat(255.0f) + F32x16T::splat(0.5f);
  rf.saveToU16x16(r);
  gf.saveToU16x16(g);
  bf.saveToU16x16(b);
  af.saveToU16x16(a);
}

#endif

void moveSourceToDestination(Pipeline& pipeline) {
  pipeline.dr = pipeline.r;
  pipeline.dg = pipeline.g;
  pipeline.db = pipeline.b;
  pipeline.da = pipeline.a;
  pipeline.nextStage();
}

void moveDestinationToSource(Pipeline& pipeline) {
  pipeline.r = pipeline.dr;
  pipeline.g = pipeline.dg;
  pipeline.b = pipeline.db;
  pipeline.a = pipeline.da;
  pipeline.nextStage();
}

void premultiply(Pipeline& pipeline) {
  pipeline.r = mulDiv255(pipeline.r, pipeline.a);
  pipeline.g = mulDiv255(pipeline.g, pipeline.a);
  pipeline.b = mulDiv255(pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void uniformColor(Pipeline& pipeline) {
  pipeline.r = pipeline.uniformR;
  pipeline.g = pipeline.uniformG;
  pipeline.b = pipeline.uniformB;
  pipeline.a = pipeline.uniformA;
  pipeline.nextStage();
}

void seedShader(Pipeline& pipeline) {
  const auto iota = F32x16T(F32x8T({0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f}),
                            F32x8T({8.5f, 9.5f, 10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 15.5f}));
  const auto x = F32x16T::splat(static_cast<float>(pipeline.dx)) + iota;
  const auto y = F32x16T::splat(static_cast<float>(pipeline.dy) + 0.5f);
  split(x, pipeline.r, pipeline.g);
  split(y, pipeline.b, pipeline.a);
  pipeline.dr = U16x16T::splat(0);
  pipeline.dg = U16x16T::splat(0);
  pipeline.db = U16x16T::splat(0);
  pipeline.da = U16x16T::splat(0);
  pipeline.nextStage();
}

void scaleU8(Pipeline& pipeline) {
  const auto data = pipeline.aaMaskCtx->copyAtXY(pipeline.dx, pipeline.dy, pipeline.tail);
  std::array<std::uint16_t, 16> cl{};
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    cl[i] = i < 2 ? static_cast<std::uint16_t>(data[i]) : 0;
  }
  U16x16T c(cl);
  pipeline.r = mulDiv255(pipeline.r, c);
  pipeline.g = mulDiv255(pipeline.g, c);
  pipeline.b = mulDiv255(pipeline.b, c);
  pipeline.a = mulDiv255(pipeline.a, c);
  pipeline.nextStage();
}

void lerpU8(Pipeline& pipeline) {
  const auto data = pipeline.aaMaskCtx->copyAtXY(pipeline.dx, pipeline.dy, pipeline.tail);
  std::array<std::uint16_t, 16> cl{};
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    cl[i] = i < 2 ? static_cast<std::uint16_t>(data[i]) : 0;
  }
  U16x16T c(cl);
  const auto invC = U16x16T::splat(255) - c;
  pipeline.r = mulAddDiv255(pipeline.dr, invC, pipeline.r, c);
  pipeline.g = mulAddDiv255(pipeline.dg, invC, pipeline.g, c);
  pipeline.b = mulAddDiv255(pipeline.db, invC, pipeline.b, c);
  pipeline.a = mulAddDiv255(pipeline.da, invC, pipeline.a, c);
  pipeline.nextStage();
}

void scale1Float(Pipeline& pipeline) {
  const auto c = fromFloat(pipeline.ctx->currentCoverage);
  pipeline.r = mulDiv255(pipeline.r, c);
  pipeline.g = mulDiv255(pipeline.g, c);
  pipeline.b = mulDiv255(pipeline.b, c);
  pipeline.a = mulDiv255(pipeline.a, c);
  pipeline.nextStage();
}

void lerp1Float(Pipeline& pipeline) {
  const auto c = fromFloat(pipeline.ctx->currentCoverage);
  const auto invC = U16x16T::splat(255) - c;
  pipeline.r = mulAddDiv255(pipeline.dr, invC, pipeline.r, c);
  pipeline.g = mulAddDiv255(pipeline.dg, invC, pipeline.g, c);
  pipeline.b = mulAddDiv255(pipeline.db, invC, pipeline.b, c);
  pipeline.a = mulAddDiv255(pipeline.da, invC, pipeline.a, c);
  pipeline.nextStage();
}

// --- Blend-mode helpers ---
//
// blendFn:  applies F(s, d, sa, da) uniformly to all four channels (r, g, b, a).
// blendFn2: applies F(s, d, sa, da) to r, g, b only; alpha uses sourceOver:
//            a' = sa + div255(da * (255 - sa)).

template <typename F>
void blendFn(Pipeline& pipeline, F&& f) {
  const auto sa = pipeline.a, da = pipeline.da;
  pipeline.r = f(pipeline.r, pipeline.dr, sa, da);
  pipeline.g = f(pipeline.g, pipeline.dg, sa, da);
  pipeline.b = f(pipeline.b, pipeline.db, sa, da);
  pipeline.a = f(sa, da, sa, da);
  pipeline.nextStage();
}

template <typename F>
void blendFn2(Pipeline& pipeline, F&& f) {
  const auto sa = pipeline.a, da = pipeline.da;
  pipeline.r = f(pipeline.r, pipeline.dr, sa, da);
  pipeline.g = f(pipeline.g, pipeline.dg, sa, da);
  pipeline.b = f(pipeline.b, pipeline.db, sa, da);
  pipeline.a = sourceOverChannel(sa, da, sa);
  pipeline.nextStage();
}

void clear(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T /*s*/, U16x16T /*d*/, U16x16T /*sa*/, U16x16T /*da*/) {
    return U16x16T::splat(0);
  });
}

void destinationAtop(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return mulAddDiv255(d, sa, s, U16x16T::splat(255) - da);
  });
}

void destinationIn(Pipeline& pipeline) {
  blendFn(pipeline,
          [](U16x16T /*s*/, U16x16T d, U16x16T sa, U16x16T /*da*/) { return mulDiv255(d, sa); });
}

void destinationOut(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T /*s*/, U16x16T d, U16x16T sa, U16x16T /*da*/) {
    return mulDiv255(d, U16x16T::splat(255) - sa);
  });
}

void sourceAtop(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return mulAddDiv255(s, da, d, U16x16T::splat(255) - sa);
  });
}

void sourceIn(Pipeline& pipeline) {
  blendFn(pipeline,
          [](U16x16T s, U16x16T /*d*/, U16x16T /*sa*/, U16x16T da) { return mulDiv255(s, da); });
}

void sourceOut(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T /*d*/, U16x16T /*sa*/, U16x16T da) {
    return mulDiv255(s, U16x16T::splat(255) - da);
  });
}

void sourceOver(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T /*da*/) {
    return sourceOverChannel(s, d, sa);
  });
}

void destinationOver(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T /*sa*/, U16x16T da) {
    return sourceOverChannel(d, s, da);
  });
}

void modulate(Pipeline& pipeline) {
  blendFn(pipeline,
          [](U16x16T s, U16x16T d, U16x16T /*sa*/, U16x16T /*da*/) { return mulDiv255(s, d); });
}

void multiply(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return div255(s * (U16x16T::splat(255) - da) + d * (U16x16T::splat(255) - sa) + s * d);
  });
}

void plus(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T /*sa*/, U16x16T /*da*/) {
    return (s + d).min(U16x16T::splat(255));
  });
}

void screen(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T /*sa*/, U16x16T /*da*/) {
    return s + d - mulDiv255(s, d);
  });
}

void xOr(Pipeline& pipeline) {
  blendFn(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return mulAddDiv255(s, U16x16T::splat(255) - da, d, U16x16T::splat(255) - sa);
  });
}

void nullFn(Pipeline& pipeline) { (void)pipeline; }

// Returns a span of PremultipliedColorU8 pixels starting at (dx, dy) in the pixmap.
inline std::span<PremultipliedColorU8> pixelsAtXY(MutableSubPixmapView& pixmap, std::size_t dx,
                                                  std::size_t dy) {
  const auto pixelOffset = dy * pixmap.realWidth + dx;
  auto* pixels = reinterpret_cast<PremultipliedColorU8*>(pixmap.data);
  const auto totalPixels = pixmap.realWidth * pixmap.size.height();
  return std::span<PremultipliedColorU8>(pixels + pixelOffset, totalPixels - pixelOffset);
}

// Loads u8 pixel channels into U16x16T (zero-extend u8 -> u16).
void load8888Lowp(std::span<const PremultipliedColorU8> pixels, U16x16T& or_, U16x16T& og,
                  U16x16T& ob, U16x16T& oa) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  if constexpr (useAarch64NeonNative()) {
    static_assert(sizeof(PremultipliedColorU8) == 4);

    const auto* packed = reinterpret_cast<const std::uint8_t*>(pixels.data());
    const uint8x16x4_t rgba = vld4q_u8(packed);
    const uint16x8x2_t r16 = {vmovl_u8(vget_low_u8(rgba.val[0])),
                              vmovl_u8(vget_high_u8(rgba.val[0]))};
    const uint16x8x2_t g16 = {vmovl_u8(vget_low_u8(rgba.val[1])),
                              vmovl_u8(vget_high_u8(rgba.val[1]))};
    const uint16x8x2_t b16 = {vmovl_u8(vget_low_u8(rgba.val[2])),
                              vmovl_u8(vget_high_u8(rgba.val[2]))};
    const uint16x8x2_t a16 = {vmovl_u8(vget_low_u8(rgba.val[3])),
                              vmovl_u8(vget_high_u8(rgba.val[3]))};
    or_ = U16x16T(r16.val[0], r16.val[1]);
    og = U16x16T(g16.val[0], g16.val[1]);
    ob = U16x16T(b16.val[0], b16.val[1]);
    oa = U16x16T(a16.val[0], a16.val[1]);
    return;
  }
#endif

#if defined(__x86_64__) || defined(__i386__)
  if constexpr (useX86Avx2FmaNative()) {
    static_assert(sizeof(PremultipliedColorU8) == 4);

    const auto* packed = reinterpret_cast<const std::uint8_t*>(pixels.data());
    const __m128i p0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + 0));
    const __m128i p1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + 16));
    const __m128i p2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + 32));
    const __m128i p3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed + 48));
    const __m128i zero = _mm_setzero_si128();

    const __m128i rMask =
        _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i gMask =
        _mm_setr_epi8(1, 5, 9, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i bMask =
        _mm_setr_epi8(2, 6, 10, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i aMask =
        _mm_setr_epi8(3, 7, 11, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);

    const auto gatherChannel = [&](__m128i mask) {
      const __m128i c0 = _mm_shuffle_epi8(p0, mask);
      const __m128i c1 = _mm_shuffle_epi8(p1, mask);
      const __m128i c2 = _mm_shuffle_epi8(p2, mask);
      const __m128i c3 = _mm_shuffle_epi8(p3, mask);
      const __m128i c01 = _mm_unpacklo_epi32(c0, c1);
      const __m128i c23 = _mm_unpacklo_epi32(c2, c3);
      return _mm_unpacklo_epi64(c01, c23);
    };

    const __m128i r8 = gatherChannel(rMask);
    const __m128i g8 = gatherChannel(gMask);
    const __m128i b8 = gatherChannel(bMask);
    const __m128i a8 = gatherChannel(aMask);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(or_.lanes().data()), _mm_unpacklo_epi8(r8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(or_.lanes().data() + 8),
                     _mm_unpackhi_epi8(r8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(og.lanes().data()), _mm_unpacklo_epi8(g8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(og.lanes().data() + 8),
                     _mm_unpackhi_epi8(g8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ob.lanes().data()), _mm_unpacklo_epi8(b8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ob.lanes().data() + 8),
                     _mm_unpackhi_epi8(b8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(oa.lanes().data()), _mm_unpacklo_epi8(a8, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(oa.lanes().data() + 8),
                     _mm_unpackhi_epi8(a8, zero));
    return;
  }
#endif

  std::array<std::uint16_t, 16> rl{}, gl{}, bl{}, al{};

  rl[0] = pixels[0].red();
  rl[1] = pixels[1].red();
  rl[2] = pixels[2].red();
  rl[3] = pixels[3].red();
  rl[4] = pixels[4].red();
  rl[5] = pixels[5].red();
  rl[6] = pixels[6].red();
  rl[7] = pixels[7].red();
  rl[8] = pixels[8].red();
  rl[9] = pixels[9].red();
  rl[10] = pixels[10].red();
  rl[11] = pixels[11].red();
  rl[12] = pixels[12].red();
  rl[13] = pixels[13].red();
  rl[14] = pixels[14].red();
  rl[15] = pixels[15].red();

  gl[0] = pixels[0].green();
  gl[1] = pixels[1].green();
  gl[2] = pixels[2].green();
  gl[3] = pixels[3].green();
  gl[4] = pixels[4].green();
  gl[5] = pixels[5].green();
  gl[6] = pixels[6].green();
  gl[7] = pixels[7].green();
  gl[8] = pixels[8].green();
  gl[9] = pixels[9].green();
  gl[10] = pixels[10].green();
  gl[11] = pixels[11].green();
  gl[12] = pixels[12].green();
  gl[13] = pixels[13].green();
  gl[14] = pixels[14].green();
  gl[15] = pixels[15].green();

  bl[0] = pixels[0].blue();
  bl[1] = pixels[1].blue();
  bl[2] = pixels[2].blue();
  bl[3] = pixels[3].blue();
  bl[4] = pixels[4].blue();
  bl[5] = pixels[5].blue();
  bl[6] = pixels[6].blue();
  bl[7] = pixels[7].blue();
  bl[8] = pixels[8].blue();
  bl[9] = pixels[9].blue();
  bl[10] = pixels[10].blue();
  bl[11] = pixels[11].blue();
  bl[12] = pixels[12].blue();
  bl[13] = pixels[13].blue();
  bl[14] = pixels[14].blue();
  bl[15] = pixels[15].blue();

  al[0] = pixels[0].alpha();
  al[1] = pixels[1].alpha();
  al[2] = pixels[2].alpha();
  al[3] = pixels[3].alpha();
  al[4] = pixels[4].alpha();
  al[5] = pixels[5].alpha();
  al[6] = pixels[6].alpha();
  al[7] = pixels[7].alpha();
  al[8] = pixels[8].alpha();
  al[9] = pixels[9].alpha();
  al[10] = pixels[10].alpha();
  al[11] = pixels[11].alpha();
  al[12] = pixels[12].alpha();
  al[13] = pixels[13].alpha();
  al[14] = pixels[14].alpha();
  al[15] = pixels[15].alpha();

  or_ = U16x16T(rl);
  og = U16x16T(gl);
  ob = U16x16T(bl);
  oa = U16x16T(al);
}

void load8888Tail(std::size_t count, std::span<const PremultipliedColorU8> pixels, U16x16T& or_,
                  U16x16T& og, U16x16T& ob, U16x16T& oa) {
  std::array<PremultipliedColorU8, kStageWidth> tmp{};
  tmp.fill(PremultipliedColorU8::transparent);
  std::copy_n(pixels.begin(), count, tmp.begin());
  load8888Lowp(tmp, or_, og, ob, oa);
}

// Stores U16x16T pixel channels to u8 pixels.
void store8888Lowp(std::span<PremultipliedColorU8> pixels, const U16x16T& r, const U16x16T& g,
                   const U16x16T& b, const U16x16T& a) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  if constexpr (useAarch64NeonNative()) {
    static_assert(sizeof(PremultipliedColorU8) == 4);

    uint8x16x4_t rgba{};
    // Rust/scalar lowp store uses u16->u8 truncation (wrap), not saturation.
    rgba.val[0] = vcombine_u8(vmovn_u16(r.neonLo()), vmovn_u16(r.neonHi()));
    rgba.val[1] = vcombine_u8(vmovn_u16(g.neonLo()), vmovn_u16(g.neonHi()));
    rgba.val[2] = vcombine_u8(vmovn_u16(b.neonLo()), vmovn_u16(b.neonHi()));
    rgba.val[3] = vcombine_u8(vmovn_u16(a.neonLo()), vmovn_u16(a.neonHi()));

    auto* packed = reinterpret_cast<std::uint8_t*>(pixels.data());
    vst4q_u8(packed, rgba);
    return;
  }
#endif

#if defined(__x86_64__) || defined(__i386__)
  if constexpr (useX86Avx2FmaNative()) {
    static_assert(sizeof(PremultipliedColorU8) == 4);

    const __m128i byteMask = _mm_set1_epi16(0x00FF);
    const __m128i rLo = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.lanes().data())), byteMask);
    const __m128i rHi = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.lanes().data() + 8)), byteMask);
    const __m128i gLo = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(g.lanes().data())), byteMask);
    const __m128i gHi = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(g.lanes().data() + 8)), byteMask);
    const __m128i bLo = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b.lanes().data())), byteMask);
    const __m128i bHi = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(b.lanes().data() + 8)), byteMask);
    const __m128i aLo = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(a.lanes().data())), byteMask);
    const __m128i aHi = _mm_and_si128(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(a.lanes().data() + 8)), byteMask);

    const __m128i r8 = _mm_packus_epi16(rLo, rHi);
    const __m128i g8 = _mm_packus_epi16(gLo, gHi);
    const __m128i b8 = _mm_packus_epi16(bLo, bHi);
    const __m128i a8 = _mm_packus_epi16(aLo, aHi);

    const __m128i rgLo = _mm_unpacklo_epi8(r8, g8);
    const __m128i rgHi = _mm_unpackhi_epi8(r8, g8);
    const __m128i baLo = _mm_unpacklo_epi8(b8, a8);
    const __m128i baHi = _mm_unpackhi_epi8(b8, a8);

    const __m128i rgba0 = _mm_unpacklo_epi16(rgLo, baLo);
    const __m128i rgba1 = _mm_unpackhi_epi16(rgLo, baLo);
    const __m128i rgba2 = _mm_unpacklo_epi16(rgHi, baHi);
    const __m128i rgba3 = _mm_unpackhi_epi16(rgHi, baHi);

    auto* packed = reinterpret_cast<std::uint8_t*>(pixels.data());
    _mm_storeu_si128(reinterpret_cast<__m128i*>(packed + 0), rgba0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(packed + 16), rgba1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(packed + 32), rgba2);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(packed + 48), rgba3);
    return;
  }
#endif

  const auto rl = r.lanes();
  const auto gl = g.lanes();
  const auto bl = b.lanes();
  const auto al = a.lanes();

  pixels[0] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[0]), static_cast<std::uint8_t>(gl[0]),
      static_cast<std::uint8_t>(bl[0]), static_cast<std::uint8_t>(al[0]));
  pixels[1] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[1]), static_cast<std::uint8_t>(gl[1]),
      static_cast<std::uint8_t>(bl[1]), static_cast<std::uint8_t>(al[1]));
  pixels[2] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[2]), static_cast<std::uint8_t>(gl[2]),
      static_cast<std::uint8_t>(bl[2]), static_cast<std::uint8_t>(al[2]));
  pixels[3] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[3]), static_cast<std::uint8_t>(gl[3]),
      static_cast<std::uint8_t>(bl[3]), static_cast<std::uint8_t>(al[3]));
  pixels[4] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[4]), static_cast<std::uint8_t>(gl[4]),
      static_cast<std::uint8_t>(bl[4]), static_cast<std::uint8_t>(al[4]));
  pixels[5] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[5]), static_cast<std::uint8_t>(gl[5]),
      static_cast<std::uint8_t>(bl[5]), static_cast<std::uint8_t>(al[5]));
  pixels[6] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[6]), static_cast<std::uint8_t>(gl[6]),
      static_cast<std::uint8_t>(bl[6]), static_cast<std::uint8_t>(al[6]));
  pixels[7] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[7]), static_cast<std::uint8_t>(gl[7]),
      static_cast<std::uint8_t>(bl[7]), static_cast<std::uint8_t>(al[7]));
  pixels[8] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[8]), static_cast<std::uint8_t>(gl[8]),
      static_cast<std::uint8_t>(bl[8]), static_cast<std::uint8_t>(al[8]));
  pixels[9] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[9]), static_cast<std::uint8_t>(gl[9]),
      static_cast<std::uint8_t>(bl[9]), static_cast<std::uint8_t>(al[9]));
  pixels[10] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[10]), static_cast<std::uint8_t>(gl[10]),
      static_cast<std::uint8_t>(bl[10]), static_cast<std::uint8_t>(al[10]));
  pixels[11] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[11]), static_cast<std::uint8_t>(gl[11]),
      static_cast<std::uint8_t>(bl[11]), static_cast<std::uint8_t>(al[11]));
  pixels[12] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[12]), static_cast<std::uint8_t>(gl[12]),
      static_cast<std::uint8_t>(bl[12]), static_cast<std::uint8_t>(al[12]));
  pixels[13] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[13]), static_cast<std::uint8_t>(gl[13]),
      static_cast<std::uint8_t>(bl[13]), static_cast<std::uint8_t>(al[13]));
  pixels[14] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[14]), static_cast<std::uint8_t>(gl[14]),
      static_cast<std::uint8_t>(bl[14]), static_cast<std::uint8_t>(al[14]));
  pixels[15] = PremultipliedColorU8::fromRgbaUnchecked(
      static_cast<std::uint8_t>(rl[15]), static_cast<std::uint8_t>(gl[15]),
      static_cast<std::uint8_t>(bl[15]), static_cast<std::uint8_t>(al[15]));
}

void store8888Tail(std::size_t count, std::span<PremultipliedColorU8> pixels, const U16x16T& r,
                   const U16x16T& g, const U16x16T& b, const U16x16T& a) {
  const auto rl = r.lanes();
  const auto gl = g.lanes();
  const auto bl = b.lanes();
  const auto al = a.lanes();
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pixels[i] = PremultipliedColorU8::fromRgbaUnchecked(
        static_cast<std::uint8_t>(rl[i]), static_cast<std::uint8_t>(gl[i]),
        static_cast<std::uint8_t>(bl[i]), static_cast<std::uint8_t>(al[i]));
    if (i + 1 == count) {
      break;
    }
  }
}

void load8Lowp(std::span<const std::uint8_t> data, U16x16T& a) {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  if constexpr (useAarch64NeonNative()) {
    const uint8x16_t src = vld1q_u8(data.data());
    a = U16x16T(vmovl_u8(vget_low_u8(src)), vmovl_u8(vget_high_u8(src)));
    return;
  }
#endif

#if defined(__x86_64__) || defined(__i386__)
  if constexpr (useX86Avx2FmaNative()) {
    const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data.data()));
    const __m128i zero = _mm_setzero_si128();
    _mm_storeu_si128(reinterpret_cast<__m128i*>(a.lanes().data()), _mm_unpacklo_epi8(src, zero));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(a.lanes().data() + 8),
                     _mm_unpackhi_epi8(src, zero));
    return;
  }
#endif

  std::array<std::uint16_t, 16> al{};
  al[0] = data[0];
  al[1] = data[1];
  al[2] = data[2];
  al[3] = data[3];
  al[4] = data[4];
  al[5] = data[5];
  al[6] = data[6];
  al[7] = data[7];
  al[8] = data[8];
  al[9] = data[9];
  al[10] = data[10];
  al[11] = data[11];
  al[12] = data[12];
  al[13] = data[13];
  al[14] = data[14];
  al[15] = data[15];
  a = U16x16T(al);
}

void load8Tail(std::size_t count, std::span<const std::uint8_t> data, U16x16T& a) {
  std::array<std::uint8_t, kStageWidth> tmp{};
  std::copy_n(data.begin(), count, tmp.begin());
  load8Lowp(tmp, a);
}

void loadDst(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  load8888Lowp(pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  pipeline.nextStage();
}

void loadDstTail(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  load8888Tail(pipeline.tail, pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  pipeline.nextStage();
}

void store(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  store8888Lowp(pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void storeTail(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  store8888Tail(pipeline.tail, pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void loadDstU8(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto offset = pipeline.dy * pipeline.pixmapDst->realWidth + pipeline.dx;
  load8Lowp(std::span<const std::uint8_t>(pipeline.pixmapDst->data + offset, kStageWidth),
            pipeline.da);
  pipeline.nextStage();
}

void loadDstU8Tail(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto offset = pipeline.dy * pipeline.pixmapDst->realWidth + pipeline.dx;
  load8Tail(pipeline.tail,
            std::span<const std::uint8_t>(pipeline.pixmapDst->data + offset, pipeline.tail),
            pipeline.da);
  pipeline.nextStage();
}

void storeU8(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto offset = pipeline.dy * pipeline.pixmapDst->realWidth + pipeline.dx;
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  if constexpr (useAarch64NeonNative()) {
    const uint8x16_t a8 =
        vcombine_u8(vmovn_u16(pipeline.a.neonLo()), vmovn_u16(pipeline.a.neonHi()));
    vst1q_u8(pipeline.pixmapDst->data + offset, a8);
    pipeline.nextStage();
    return;
  }
#endif
  const auto al = pipeline.a.lanes();

#if defined(__x86_64__) || defined(__i386__)
  if constexpr (useX86Avx2FmaNative()) {
    const __m128i byteMask = _mm_set1_epi16(0x00FF);
    const __m128i aLo =
        _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(al.data())), byteMask);
    const __m128i aHi =
        _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(al.data() + 8)), byteMask);
    const __m128i a8 = _mm_packus_epi16(aLo, aHi);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(pipeline.pixmapDst->data + offset), a8);
    pipeline.nextStage();
    return;
  }
#endif

  pipeline.pixmapDst->data[offset + 0] = static_cast<std::uint8_t>(al[0]);
  pipeline.pixmapDst->data[offset + 1] = static_cast<std::uint8_t>(al[1]);
  pipeline.pixmapDst->data[offset + 2] = static_cast<std::uint8_t>(al[2]);
  pipeline.pixmapDst->data[offset + 3] = static_cast<std::uint8_t>(al[3]);
  pipeline.pixmapDst->data[offset + 4] = static_cast<std::uint8_t>(al[4]);
  pipeline.pixmapDst->data[offset + 5] = static_cast<std::uint8_t>(al[5]);
  pipeline.pixmapDst->data[offset + 6] = static_cast<std::uint8_t>(al[6]);
  pipeline.pixmapDst->data[offset + 7] = static_cast<std::uint8_t>(al[7]);
  pipeline.pixmapDst->data[offset + 8] = static_cast<std::uint8_t>(al[8]);
  pipeline.pixmapDst->data[offset + 9] = static_cast<std::uint8_t>(al[9]);
  pipeline.pixmapDst->data[offset + 10] = static_cast<std::uint8_t>(al[10]);
  pipeline.pixmapDst->data[offset + 11] = static_cast<std::uint8_t>(al[11]);
  pipeline.pixmapDst->data[offset + 12] = static_cast<std::uint8_t>(al[12]);
  pipeline.pixmapDst->data[offset + 13] = static_cast<std::uint8_t>(al[13]);
  pipeline.pixmapDst->data[offset + 14] = static_cast<std::uint8_t>(al[14]);
  pipeline.pixmapDst->data[offset + 15] = static_cast<std::uint8_t>(al[15]);
  pipeline.nextStage();
}

void storeU8Tail(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  const auto offset = pipeline.dy * pipeline.pixmapDst->realWidth + pipeline.dx;
  const auto al = pipeline.a.lanes();
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.pixmapDst->data[offset + i] = static_cast<std::uint8_t>(al[i]);
    if (i + 1 == pipeline.tail) {
      break;
    }
  }
  pipeline.nextStage();
}

void loadMaskU8(Pipeline& pipeline) {
  if (pipeline.maskCtx == nullptr || pipeline.maskCtx->data == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto offset = pipeline.maskCtx->byteOffset(pipeline.dx, pipeline.dy);
  std::array<std::uint16_t, 16> al{};
  for (std::size_t i = 0; i < pipeline.tail; ++i) {
    al[i] = static_cast<std::uint16_t>(pipeline.maskCtx->data[offset + i]);
  }
  pipeline.a = U16x16T(al);
  pipeline.r = U16x16T::splat(0);
  pipeline.g = U16x16T::splat(0);
  pipeline.b = U16x16T::splat(0);
  pipeline.nextStage();
}

void maskU8(Pipeline& pipeline) {
  if (pipeline.maskCtx == nullptr || pipeline.maskCtx->data == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto offset = pipeline.maskCtx->byteOffset(pipeline.dx, pipeline.dy);
  std::array<std::uint16_t, 16> cl{};
  for (std::size_t i = 0; i < pipeline.tail; ++i) {
    cl[i] = static_cast<std::uint16_t>(pipeline.maskCtx->data[offset + i]);
  }
  bool allZero = true;
  for (std::size_t i = 0; i < pipeline.tail; ++i) {
    if (cl[i] != 0) {
      allZero = false;
      break;
    }
  }
  U16x16T c(cl);
  if (allZero) {
    return;
  }
  pipeline.r = mulDiv255(pipeline.r, c);
  pipeline.g = mulDiv255(pipeline.g, c);
  pipeline.b = mulDiv255(pipeline.b, c);
  pipeline.a = mulDiv255(pipeline.a, c);
  pipeline.nextStage();
}

void sourceOverRgba(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  load8888Lowp(pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  pipeline.r = sourceOverChannel(pipeline.r, pipeline.dr, pipeline.a);
  pipeline.g = sourceOverChannel(pipeline.g, pipeline.dg, pipeline.a);
  pipeline.b = sourceOverChannel(pipeline.b, pipeline.db, pipeline.a);
  pipeline.a = sourceOverChannel(pipeline.a, pipeline.da, pipeline.a);
  store8888Lowp(pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void sourceOverRgbaTail(Pipeline& pipeline) {
  assert(pipeline.pixmapDst != nullptr);
  auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
  load8888Tail(pipeline.tail, pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  pipeline.r = sourceOverChannel(pipeline.r, pipeline.dr, pipeline.a);
  pipeline.g = sourceOverChannel(pipeline.g, pipeline.dg, pipeline.a);
  pipeline.b = sourceOverChannel(pipeline.b, pipeline.db, pipeline.a);
  pipeline.a = sourceOverChannel(pipeline.a, pipeline.da, pipeline.a);
  store8888Tail(pipeline.tail, pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void darken(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return s + d - div255((s * da).max(d * sa));
  });
}

void lighten(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return s + d - div255((s * da).min(d * sa));
  });
}

void difference(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    return s + d - U16x16T::splat(2) * div255((s * da).min(d * sa));
  });
}

void exclusion(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T /*sa*/, U16x16T /*da*/) {
    return s + d - U16x16T::splat(2) * div255(s * d);
  });
}

void hardLight(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    const auto invDa = U16x16T::splat(255) - da;
    const auto invSa = U16x16T::splat(255) - sa;
    const auto bodyIf = U16x16T::splat(2) * s * d;
    const auto bodyElse = sa * da - U16x16T::splat(2) * (sa - s) * (da - d);
    const auto mask = (U16x16T::splat(2) * s).cmpLe(sa);
    const auto body = mask.blend(bodyIf, bodyElse);
    return div255(s * invDa + d * invSa + body);
  });
}

void overlay(Pipeline& pipeline) {
  blendFn2(pipeline, [](U16x16T s, U16x16T d, U16x16T sa, U16x16T da) {
    const auto invDa = U16x16T::splat(255) - da;
    const auto invSa = U16x16T::splat(255) - sa;
    const auto bodyIf = U16x16T::splat(2) * s * d;
    const auto bodyElse = sa * da - U16x16T::splat(2) * (sa - s) * (da - d);
    const auto mask = (U16x16T::splat(2) * d).cmpLe(da);
    const auto body = mask.blend(bodyIf, bodyElse);
    return div255(s * invDa + d * invSa + body);
  });
}

// --- Lowp coordinate/shader stages ---
//
// Rust lowp uses split()/join() to convert between f32x16 (a single 512-bit
// float SIMD vector used for coordinates) and pairs of u16x16 (256-bit integer
// SIMD vectors used for pixel channels). The pipeline stores coordinates by
// splitting one f32x16 across two u16x16 registers (x -> r,g and y -> b,a),
// then join() reconstructs the f32x16 before coordinate math.

void transform(Pipeline& pipeline) {
  const auto& ts = pipeline.ctx->transform;
  auto x = join(pipeline.r, pipeline.g);
  auto y = join(pipeline.b, pipeline.a);
  // Nested multiply-add: x * sx + (y * kx + tx)
  auto nx = x * F32x16T::splat(ts.sx) + (y * F32x16T::splat(ts.kx) + F32x16T::splat(ts.tx));
  auto ny = x * F32x16T::splat(ts.ky) + (y * F32x16T::splat(ts.sy) + F32x16T::splat(ts.ty));
  split(nx, pipeline.r, pipeline.g);
  split(ny, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void padX1(Pipeline& pipeline) {
  auto x = join(pipeline.r, pipeline.g);
  split(x.normalize(), pipeline.r, pipeline.g);
  pipeline.nextStage();
}

void reflectX1(Pipeline& pipeline) {
  auto x = join(pipeline.r, pipeline.g);
  auto vMinus1 = x - F32x16T::splat(1.0f);
  auto result = (vMinus1 - F32x16T::splat(2.0f) * (vMinus1 * F32x16T::splat(0.5f)).floor() -
                 F32x16T::splat(1.0f))
                    .abs()
                    .normalize();
  split(result, pipeline.r, pipeline.g);
  pipeline.nextStage();
}

void repeatX1(Pipeline& pipeline) {
  auto x = join(pipeline.r, pipeline.g);
  auto result = (x - x.floor()).normalize();
  split(result, pipeline.r, pipeline.g);
  pipeline.nextStage();
}

void evenlySpaced2StopGradient(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->evenlySpaced2StopGradient;
  auto t = join(pipeline.r, pipeline.g);
  auto rf = t * F32x16T::splat(ctx.factor.r) + F32x16T::splat(ctx.bias.r);
  auto gf = t * F32x16T::splat(ctx.factor.g) + F32x16T::splat(ctx.bias.g);
  auto bf = t * F32x16T::splat(ctx.factor.b) + F32x16T::splat(ctx.bias.b);
  auto af = t * F32x16T::splat(ctx.factor.a) + F32x16T::splat(ctx.bias.a);
  roundF32ToU16(rf, gf, bf, af, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void fusedLinearGradient2Stop(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->fusedLinearGradient2Stop;

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  // NEON-optimized: keep all values in float32x4_t registers throughout.
  // Processes 16 pixels as 4 groups of 4, avoiding array<float,8> round-trips.
  const float32x4_t vSx = vdupq_n_f32(ctx.sx);
  const float32x4_t vKx = vdupq_n_f32(ctx.kx);
  const float32x4_t vTx = vdupq_n_f32(ctx.tx);
  const float32x4_t vZero = vdupq_n_f32(0.0f);
  const float32x4_t vOne = vdupq_n_f32(1.0f);
  const float32x4_t vFr = vdupq_n_f32(ctx.factor.r);
  const float32x4_t vFg = vdupq_n_f32(ctx.factor.g);
  const float32x4_t vFb = vdupq_n_f32(ctx.factor.b);
  const float32x4_t vFa = vdupq_n_f32(ctx.factor.a);
  const float32x4_t vBr = vdupq_n_f32(ctx.bias.r);
  const float32x4_t vBg = vdupq_n_f32(ctx.bias.g);
  const float32x4_t vBb = vdupq_n_f32(ctx.bias.b);
  const float32x4_t vBa = vdupq_n_f32(ctx.bias.a);
  const float32x4_t v255 = vdupq_n_f32(255.0f);
  const float32x4_t vHalf = vdupq_n_f32(0.5f);
  const float dxf = static_cast<float>(pipeline.dx);
  const float32x4_t vy = vdupq_n_f32(static_cast<float>(pipeline.dy) + 0.5f);

  // 4 groups of 4 pixels = 16 pixels total.
  uint16x8_t rOut[2], gOut[2], bOut[2], aOut[2];
  const float32x4_t iotas[4] = {
      {dxf + 0.5f, dxf + 1.5f, dxf + 2.5f, dxf + 3.5f},
      {dxf + 4.5f, dxf + 5.5f, dxf + 6.5f, dxf + 7.5f},
      {dxf + 8.5f, dxf + 9.5f, dxf + 10.5f, dxf + 11.5f},
      {dxf + 12.5f, dxf + 13.5f, dxf + 14.5f, dxf + 15.5f},
  };

  for (int g = 0; g < 4; ++g) {
    // t = clamp(sx*x + kx*y + tx, 0, 1)
    float32x4_t t = vfmaq_f32(vfmaq_f32(vTx, iotas[g], vSx), vy, vKx);
    t = vmaxq_f32(vZero, vminq_f32(vOne, t));
    // color = t * factor + bias
    float32x4_t r = vfmaq_f32(vBr, t, vFr);
    float32x4_t gv = vfmaq_f32(vBg, t, vFg);
    float32x4_t b = vfmaq_f32(vBb, t, vFb);
    float32x4_t a = vfmaq_f32(vBa, t, vFa);
    // Premultiply if needed.
    if (ctx.needsPremultiply) {
      r = vmulq_f32(r, a);
      gv = vmulq_f32(gv, a);
      b = vmulq_f32(b, a);
    }
    // Clamp RGB to [0,1], scale to [0,255], truncate to u16.
    r = vmaxq_f32(vZero, vminq_f32(vOne, r));
    gv = vmaxq_f32(vZero, vminq_f32(vOne, gv));
    b = vmaxq_f32(vZero, vminq_f32(vOne, b));
    r = vfmaq_f32(vHalf, r, v255);
    gv = vfmaq_f32(vHalf, gv, v255);
    b = vfmaq_f32(vHalf, b, v255);
    a = vfmaq_f32(vHalf, a, v255);
    // Convert to u16: float→u32→narrow to u16
    uint32x4_t ru32 = vcvtq_u32_f32(r);
    uint32x4_t gu32 = vcvtq_u32_f32(gv);
    uint32x4_t bu32 = vcvtq_u32_f32(b);
    uint32x4_t au32 = vcvtq_u32_f32(a);
    uint16x4_t rn = vmovn_u32(ru32);
    uint16x4_t gn = vmovn_u32(gu32);
    uint16x4_t bn = vmovn_u32(bu32);
    uint16x4_t an = vmovn_u32(au32);
    // Store into the two u16x8 halves (g=0,1 → half 0; g=2,3 → half 1).
    if (g == 0) {
      rOut[0] = vcombine_u16(rn, vdup_n_u16(0));
      gOut[0] = vcombine_u16(gn, vdup_n_u16(0));
      bOut[0] = vcombine_u16(bn, vdup_n_u16(0));
      aOut[0] = vcombine_u16(an, vdup_n_u16(0));
    } else if (g == 1) {
      rOut[0] = vcombine_u16(vget_low_u16(rOut[0]), rn);
      gOut[0] = vcombine_u16(vget_low_u16(gOut[0]), gn);
      bOut[0] = vcombine_u16(vget_low_u16(bOut[0]), bn);
      aOut[0] = vcombine_u16(vget_low_u16(aOut[0]), an);
    } else if (g == 2) {
      rOut[1] = vcombine_u16(rn, vdup_n_u16(0));
      gOut[1] = vcombine_u16(gn, vdup_n_u16(0));
      bOut[1] = vcombine_u16(bn, vdup_n_u16(0));
      aOut[1] = vcombine_u16(an, vdup_n_u16(0));
    } else {
      rOut[1] = vcombine_u16(vget_low_u16(rOut[1]), rn);
      gOut[1] = vcombine_u16(vget_low_u16(gOut[1]), gn);
      bOut[1] = vcombine_u16(vget_low_u16(bOut[1]), bn);
      aOut[1] = vcombine_u16(vget_low_u16(aOut[1]), an);
    }
  }
  pipeline.r = U16x16T(rOut[0], rOut[1]);
  pipeline.g = U16x16T(gOut[0], gOut[1]);
  pipeline.b = U16x16T(bOut[0], bOut[1]);
  pipeline.a = U16x16T(aOut[0], aOut[1]);

#else
  // Generic fallback using F32x16T.
  const auto iota = F32x16T(F32x8T({0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f}),
                            F32x8T({8.5f, 9.5f, 10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 15.5f}));
  auto x = F32x16T::splat(static_cast<float>(pipeline.dx)) + iota;
  auto y = F32x16T::splat(static_cast<float>(pipeline.dy) + 0.5f);
  auto t = (x * F32x16T::splat(ctx.sx) + y * F32x16T::splat(ctx.kx) +
            F32x16T::splat(ctx.tx))
               .normalize();
  auto rf = t * F32x16T::splat(ctx.factor.r) + F32x16T::splat(ctx.bias.r);
  auto gf = t * F32x16T::splat(ctx.factor.g) + F32x16T::splat(ctx.bias.g);
  auto bf = t * F32x16T::splat(ctx.factor.b) + F32x16T::splat(ctx.bias.b);
  auto af = t * F32x16T::splat(ctx.factor.a) + F32x16T::splat(ctx.bias.a);
  if (ctx.needsPremultiply) {
    rf = rf * af;
    gf = gf * af;
    bf = bf * af;
  }
  roundF32ToU16(rf, gf, bf, af, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
#endif

  pipeline.nextStage();
}

void gradient(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->gradient;
  auto t = join(pipeline.r, pipeline.g);

  // Per-lane index lookup — inherently scalar
  auto tLo = t.lo().lanes();
  auto tHi = t.hi().lanes();

  std::array<float, 8> rLo{}, gLo{}, bLo{}, aLo{};
  std::array<float, 8> rHi{}, gHi{}, bHi{}, aHi{};

  auto process = [&](const std::array<float, 8>& tv, std::array<float, 8>& rv,
                     std::array<float, 8>& gv, std::array<float, 8>& bv, std::array<float, 8>& av) {
    for (std::size_t i = 0; i < 8; ++i) {
      std::uint32_t idx = 0;
      for (std::size_t s = 1; s < ctx.len; ++s) {
        if (tv[i] >= ctx.tValues[s]) {
          idx += 1;
        }
      }
      const auto& factor = ctx.factors[idx];
      const auto& bias = ctx.biases[idx];
      rv[i] = tv[i] * factor.r + bias.r;
      gv[i] = tv[i] * factor.g + bias.g;
      bv[i] = tv[i] * factor.b + bias.b;
      av[i] = tv[i] * factor.a + bias.a;
    }
  };

  process(tLo, rLo, gLo, bLo, aLo);
  process(tHi, rHi, gHi, bHi, aHi);

  auto rf = F32x16T(F32x8T(rLo), F32x8T(rHi));
  auto gf = F32x16T(F32x8T(gLo), F32x8T(gHi));
  auto bf = F32x16T(F32x8T(bLo), F32x8T(bHi));
  auto af = F32x16T(F32x8T(aLo), F32x8T(aHi));

  roundF32ToU16(rf, gf, bf, af, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

void xyToRadius(Pipeline& pipeline) {
  auto x = join(pipeline.r, pipeline.g);
  auto y = join(pipeline.b, pipeline.a);
  auto radius = (x * x + y * y).sqrt();
  split(radius, pipeline.r, pipeline.g);
  pipeline.nextStage();
}

namespace {

/// Bilinear sample helper: loads one pixel from RGBA data, returns float RGBA.
inline void loadPixelRGBA(const std::uint8_t* data, std::uint32_t idx, float& r, float& g,
                          float& b, float& a) {
  const std::size_t off = static_cast<std::size_t>(idx) * 4;
  r = static_cast<float>(data[off + 0]) * (1.0f / 255.0f);
  g = static_cast<float>(data[off + 1]) * (1.0f / 255.0f);
  b = static_cast<float>(data[off + 2]) * (1.0f / 255.0f);
  a = static_cast<float>(data[off + 3]) * (1.0f / 255.0f);
}

/// Repeat tiling: wraps v into [0, limit).
inline float repeatTile(float v, float limit, float invLimit) {
  return v - std::floor(v * invLimit) * limit;
}

/// Clamp to [0, maxVal].
inline float clampCoord(float v, float maxVal) {
  return std::max(0.0f, std::min(maxVal, v));
}

}  // namespace

void fusedRadialGradient2Stop(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->fusedRadialGradient2Stop;

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  // NEON-optimized: full transform + sqrt + color interpolation, all in registers.
  const float32x4_t vSx = vdupq_n_f32(ctx.sx);
  const float32x4_t vKx = vdupq_n_f32(ctx.kx);
  const float32x4_t vTx = vdupq_n_f32(ctx.tx);
  const float32x4_t vKy = vdupq_n_f32(ctx.ky);
  const float32x4_t vSy = vdupq_n_f32(ctx.sy);
  const float32x4_t vTy = vdupq_n_f32(ctx.ty);
  const float32x4_t vZero = vdupq_n_f32(0.0f);
  const float32x4_t vOne = vdupq_n_f32(1.0f);
  const float32x4_t vFr = vdupq_n_f32(ctx.factor.r);
  const float32x4_t vFg = vdupq_n_f32(ctx.factor.g);
  const float32x4_t vFb = vdupq_n_f32(ctx.factor.b);
  const float32x4_t vFa = vdupq_n_f32(ctx.factor.a);
  const float32x4_t vBr = vdupq_n_f32(ctx.bias.r);
  const float32x4_t vBg = vdupq_n_f32(ctx.bias.g);
  const float32x4_t vBb = vdupq_n_f32(ctx.bias.b);
  const float32x4_t vBa = vdupq_n_f32(ctx.bias.a);
  const float32x4_t v255 = vdupq_n_f32(255.0f);
  const float32x4_t vHalf = vdupq_n_f32(0.5f);
  const float dxf = static_cast<float>(pipeline.dx);
  const float32x4_t vy = vdupq_n_f32(static_cast<float>(pipeline.dy) + 0.5f);

  uint16x8_t rOut[2], gOut[2], bOut[2], aOut[2];
  const float32x4_t iotas[4] = {
      {dxf + 0.5f, dxf + 1.5f, dxf + 2.5f, dxf + 3.5f},
      {dxf + 4.5f, dxf + 5.5f, dxf + 6.5f, dxf + 7.5f},
      {dxf + 8.5f, dxf + 9.5f, dxf + 10.5f, dxf + 11.5f},
      {dxf + 12.5f, dxf + 13.5f, dxf + 14.5f, dxf + 15.5f},
  };

  for (int g = 0; g < 4; ++g) {
    // Transform to unit space: u = sx*x + kx*y + tx, v = ky*x + sy*y + ty
    float32x4_t u = vfmaq_f32(vfmaq_f32(vTx, iotas[g], vSx), vy, vKx);
    float32x4_t v = vfmaq_f32(vfmaq_f32(vTy, iotas[g], vKy), vy, vSy);
    // Radius = sqrt(u*u + v*v), clamped to [0,1]
    float32x4_t t = vsqrtq_f32(vfmaq_f32(vmulq_f32(u, u), v, v));
    t = vmaxq_f32(vZero, vminq_f32(vOne, t));
    // Color interpolation: color = t * factor + bias
    float32x4_t r = vfmaq_f32(vBr, t, vFr);
    float32x4_t gv = vfmaq_f32(vBg, t, vFg);
    float32x4_t b = vfmaq_f32(vBb, t, vFb);
    float32x4_t a = vfmaq_f32(vBa, t, vFa);
    if (ctx.needsPremultiply) {
      r = vmulq_f32(r, a);
      gv = vmulq_f32(gv, a);
      b = vmulq_f32(b, a);
    }
    // Clamp, scale to [0,255], truncate to u16.
    r = vmaxq_f32(vZero, vminq_f32(vOne, r));
    gv = vmaxq_f32(vZero, vminq_f32(vOne, gv));
    b = vmaxq_f32(vZero, vminq_f32(vOne, b));
    r = vfmaq_f32(vHalf, r, v255);
    gv = vfmaq_f32(vHalf, gv, v255);
    b = vfmaq_f32(vHalf, b, v255);
    a = vfmaq_f32(vHalf, a, v255);
    uint32x4_t ru32 = vcvtq_u32_f32(r);
    uint32x4_t gu32 = vcvtq_u32_f32(gv);
    uint32x4_t bu32 = vcvtq_u32_f32(b);
    uint32x4_t au32 = vcvtq_u32_f32(a);
    uint16x4_t rn = vmovn_u32(ru32);
    uint16x4_t gn = vmovn_u32(gu32);
    uint16x4_t bn = vmovn_u32(bu32);
    uint16x4_t an = vmovn_u32(au32);
    if (g == 0) {
      rOut[0] = vcombine_u16(rn, vdup_n_u16(0));
      gOut[0] = vcombine_u16(gn, vdup_n_u16(0));
      bOut[0] = vcombine_u16(bn, vdup_n_u16(0));
      aOut[0] = vcombine_u16(an, vdup_n_u16(0));
    } else if (g == 1) {
      rOut[0] = vcombine_u16(vget_low_u16(rOut[0]), rn);
      gOut[0] = vcombine_u16(vget_low_u16(gOut[0]), gn);
      bOut[0] = vcombine_u16(vget_low_u16(bOut[0]), bn);
      aOut[0] = vcombine_u16(vget_low_u16(aOut[0]), an);
    } else if (g == 2) {
      rOut[1] = vcombine_u16(rn, vdup_n_u16(0));
      gOut[1] = vcombine_u16(gn, vdup_n_u16(0));
      bOut[1] = vcombine_u16(bn, vdup_n_u16(0));
      aOut[1] = vcombine_u16(an, vdup_n_u16(0));
    } else {
      rOut[1] = vcombine_u16(vget_low_u16(rOut[1]), rn);
      gOut[1] = vcombine_u16(vget_low_u16(gOut[1]), gn);
      bOut[1] = vcombine_u16(vget_low_u16(bOut[1]), bn);
      aOut[1] = vcombine_u16(vget_low_u16(aOut[1]), an);
    }
  }
  pipeline.r = U16x16T(rOut[0], rOut[1]);
  pipeline.g = U16x16T(gOut[0], gOut[1]);
  pipeline.b = U16x16T(bOut[0], bOut[1]);
  pipeline.a = U16x16T(aOut[0], aOut[1]);

#else
  // Generic fallback using F32x16T.
  const auto iota = F32x16T(F32x8T({0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f}),
                            F32x8T({8.5f, 9.5f, 10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 15.5f}));
  auto x = F32x16T::splat(static_cast<float>(pipeline.dx)) + iota;
  auto y = F32x16T::splat(static_cast<float>(pipeline.dy) + 0.5f);
  auto u = x * F32x16T::splat(ctx.sx) + y * F32x16T::splat(ctx.kx) + F32x16T::splat(ctx.tx);
  auto v = x * F32x16T::splat(ctx.ky) + y * F32x16T::splat(ctx.sy) + F32x16T::splat(ctx.ty);
  auto t = (u * u + v * v).sqrt().normalize();
  auto rf = t * F32x16T::splat(ctx.factor.r) + F32x16T::splat(ctx.bias.r);
  auto gf = t * F32x16T::splat(ctx.factor.g) + F32x16T::splat(ctx.bias.g);
  auto bf = t * F32x16T::splat(ctx.factor.b) + F32x16T::splat(ctx.bias.b);
  auto af = t * F32x16T::splat(ctx.factor.a) + F32x16T::splat(ctx.bias.a);
  if (ctx.needsPremultiply) {
    rf = rf * af;
    gf = gf * af;
    bf = bf * af;
  }
  roundF32ToU16(rf, gf, bf, af, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
#endif

  pipeline.nextStage();
}

/// Reflect tiling for coordinates in [0, limit) range.
inline float exclusiveReflect(float v, float limit, float invLimit) {
  return std::abs((v - limit) - (limit + limit) * std::floor((v - limit) * (invLimit * 0.5f)) -
                  limit);
}

void fusedBilinearPattern(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->fusedBilinearPattern;
  if (!ctx.pixels || ctx.width == 0 || ctx.height == 0) {
    pipeline.nextStage();
    return;
  }

  const std::uint32_t w = ctx.width;
  const std::uint32_t h = ctx.height;
  const std::uint32_t pixCount = w * h;
  const float fw = static_cast<float>(w);
  const float fh = static_cast<float>(h);
  const float wMax = fw - 1.0f;
  const float hMax = fh - 1.0f;

  if (ctx.useNearest) {
    // Nearest-neighbor: direct u8→u16, no float intermediate.
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
    uint16x8_t rLo, rHi, gLo, gHi, bLo, bHi, aLo, aHi;

    // Fast path: unit-stride in x (identity-like transform) — consecutive output pixels
    // map to consecutive tile pixels, enabling bulk vld4q_u8 deinterleaved loads.
    const bool isUnitXStride = (ctx.sx == 1.0f && ctx.kx == 0.0f && ctx.ky == 0.0f);

    // Scanline-level fast path: process entire row at once, eliminating per-batch dispatch.
    if (isUnitXStride && ctx.spreadMode == SpreadMode::Repeat && ctx.fuseSourceOver) {
      auto& mutCtx = pipeline.ctx->fusedBilinearPattern;

      // Tile row (constant for entire scanline).
      const float cyRaw = (static_cast<float>(pipeline.dy) + 0.5f) * ctx.sy + ctx.ty;
      float cyTiled = cyRaw * ctx.invHeight;
      cyTiled = (cyTiled - std::floor(cyTiled)) * fh;
      if (cyTiled < 0.0f) cyTiled = 0.0f;
      if (cyTiled > hMax) cyTiled = hMax;
      const std::uint32_t tileY = static_cast<std::uint32_t>(static_cast<std::int32_t>(cyTiled));
      const std::uint8_t* row = ctx.pixels + static_cast<std::size_t>(tileY) * w * 4;

      // Initial tile X.
      float cxInit = (static_cast<float>(pipeline.rect->x()) + 0.5f) + ctx.tx;
      cxInit = cxInit - std::floor(cxInit * ctx.invWidth) * fw;
      if (cxInit < 0.0f) cxInit += fw;
      std::uint32_t tileX = std::min(
          static_cast<std::uint32_t>(static_cast<std::int32_t>(cxInit)), w - 1);

      // Destination row.
      auto* dstRow = reinterpret_cast<std::uint8_t*>(pipeline.pixmapDst->data) +
          pipeline.dy * static_cast<std::size_t>(pipeline.pixmapDst->realWidth) * 4 +
          pipeline.rect->x() * 4;

      const std::size_t scanWidth = pipeline.rect->right() - pipeline.rect->x();
      const bool needOpacity = ctx.opacity < 1.0f;
      const uint8x16_t vopac8 = vdupq_n_u8(
          static_cast<std::uint8_t>(std::min(255.0f, ctx.opacity * 255.0f + 0.5f)));
      const uint8x16_t v255_8 = vdupq_n_u8(255);

      // One-time check: is the entire tile fully opaque (all alpha=255)?
      // If so, SourceOver reduces to a direct copy — skip blend entirely.
      if (mutCtx.opaqueCheckResult < 0) {
        const std::size_t tilePixels = static_cast<std::size_t>(w) * h;
        bool allOpaque = !needOpacity;
        if (allOpaque) {
          for (std::size_t i = 0; i + 16 <= tilePixels && allOpaque; i += 16) {
            uint8x16x4_t px = vld4q_u8(ctx.pixels + i * 4);
            if (vminvq_u8(px.val[3]) != 255) allOpaque = false;
          }
          // Check remaining pixels.
          for (std::size_t i = (tilePixels / 16) * 16; i < tilePixels && allOpaque; ++i) {
            if (ctx.pixels[i * 4 + 3] != 255) allOpaque = false;
          }
        }
        mutCtx.opaqueCheckResult = allOpaque ? 1 : 0;
      }
      const bool tileIsOpaque = (mutCtx.opaqueCheckResult == 1);

      // Process in tile-aligned segments (no wrapping, no modulo).
      std::size_t dstOff = 0;
      std::size_t rem = scanWidth;
      while (rem > 0) {
        std::size_t run = std::min(rem, static_cast<std::size_t>(w - tileX));

        if (tileIsOpaque) {
          // Opaque fast path: direct memcpy from tile to dest (no blend needed).
          std::memcpy(dstRow + dstOff, row + tileX * 4, run * 4);
          dstOff += run * 4;
          tileX += static_cast<std::uint32_t>(run);
          rem -= run;
        } else {
          // SourceOver blend path.
          while (run >= 16) {
            uint8x16x4_t src = vld4q_u8(row + tileX * 4);
            if (needOpacity) {
              auto sc = [&](uint8x16_t v) {
                return vcombine_u8(
                    vrshrn_n_u16(vmull_u8(vget_low_u8(v), vget_low_u8(vopac8)), 8),
                    vrshrn_n_u16(vmull_u8(vget_high_u8(v), vget_high_u8(vopac8)), 8));
              };
              src.val[0] = sc(src.val[0]); src.val[1] = sc(src.val[1]);
              src.val[2] = sc(src.val[2]); src.val[3] = sc(src.val[3]);
            }
            auto* dp = dstRow + dstOff;
            const uint8x16x4_t d = vld4q_u8(dp);
            const uint8x16_t invA = vsubq_u8(v255_8, src.val[3]);
            uint8x16x4_t out;
            out.val[0] = vcombine_u8(
                vadd_u8(vget_low_u8(src.val[0]),
                    vrshrn_n_u16(vmull_u8(vget_low_u8(d.val[0]), vget_low_u8(invA)), 8)),
                vadd_u8(vget_high_u8(src.val[0]),
                    vrshrn_n_u16(vmull_u8(vget_high_u8(d.val[0]), vget_high_u8(invA)), 8)));
            out.val[1] = vcombine_u8(
                vadd_u8(vget_low_u8(src.val[1]),
                    vrshrn_n_u16(vmull_u8(vget_low_u8(d.val[1]), vget_low_u8(invA)), 8)),
                vadd_u8(vget_high_u8(src.val[1]),
                    vrshrn_n_u16(vmull_u8(vget_high_u8(d.val[1]), vget_high_u8(invA)), 8)));
            out.val[2] = vcombine_u8(
                vadd_u8(vget_low_u8(src.val[2]),
                    vrshrn_n_u16(vmull_u8(vget_low_u8(d.val[2]), vget_low_u8(invA)), 8)),
                vadd_u8(vget_high_u8(src.val[2]),
                    vrshrn_n_u16(vmull_u8(vget_high_u8(d.val[2]), vget_high_u8(invA)), 8)));
            out.val[3] = vcombine_u8(
                vadd_u8(vget_low_u8(src.val[3]),
                    vrshrn_n_u16(vmull_u8(vget_low_u8(d.val[3]), vget_low_u8(invA)), 8)),
                vadd_u8(vget_high_u8(src.val[3]),
                    vrshrn_n_u16(vmull_u8(vget_high_u8(d.val[3]), vget_high_u8(invA)), 8)));
            vst4q_u8(dp, out);
            tileX += 16;
            dstOff += 64;
            run -= 16;
            rem -= 16;
          }
          while (run > 0) {
            const std::uint8_t* sp = row + tileX * 4;
            auto* dp = dstRow + dstOff;
            std::uint16_t sr = sp[0], sg = sp[1], sb = sp[2], sa = sp[3];
            if (needOpacity) {
              const auto op = static_cast<std::uint16_t>(ctx.opacity * 255.0f + 0.5f);
              sr = static_cast<std::uint16_t>((sr * op + 128) >> 8);
              sg = static_cast<std::uint16_t>((sg * op + 128) >> 8);
              sb = static_cast<std::uint16_t>((sb * op + 128) >> 8);
              sa = static_cast<std::uint16_t>((sa * op + 128) >> 8);
            }
            std::uint16_t inv = 255 - sa;
            dp[0] = static_cast<std::uint8_t>(sr + ((dp[0] * inv + 128) >> 8));
            dp[1] = static_cast<std::uint8_t>(sg + ((dp[1] * inv + 128) >> 8));
            dp[2] = static_cast<std::uint8_t>(sb + ((dp[2] * inv + 128) >> 8));
            dp[3] = static_cast<std::uint8_t>(sa + ((dp[3] * inv + 128) >> 8));
            ++tileX;
            dstOff += 4;
            --run;
            --rem;
          }
        }
        if (tileX >= w) tileX = 0;
      }

      pipeline.scanlineDone = true;
      return;
    }

    if (isUnitXStride && ctx.spreadMode == SpreadMode::Repeat) {
      // Compute tile row (constant for all 16 pixels).
      const float cyRaw = (static_cast<float>(pipeline.dy) + 0.5f) * ctx.sy + ctx.ty;
      float cyTiled = cyRaw * ctx.invHeight;
      cyTiled = (cyTiled - std::floor(cyTiled)) * fh;
      if (cyTiled < 0.0f) cyTiled = 0.0f;
      if (cyTiled > hMax) cyTiled = hMax;
      const std::uint32_t tileY = static_cast<std::uint32_t>(static_cast<std::int32_t>(cyTiled));
      const std::uint8_t* row = ctx.pixels + static_cast<std::size_t>(tileY) * w * 4;

      // Compute starting tile X.
      const float cxRaw = (static_cast<float>(pipeline.dx) + 0.5f) + ctx.tx;
      float cxTiled = cxRaw * ctx.invWidth;
      cxTiled = (cxTiled - std::floor(cxTiled)) * fw;
      if (cxTiled < 0.0f) cxTiled = 0.0f;
      if (cxTiled > wMax) cxTiled = wMax;
      const std::uint32_t startX = static_cast<std::uint32_t>(static_cast<std::int32_t>(cxTiled));

      if (startX + 16 <= w) {
        // No wrapping — single vld4q_u8 directly from tile row.
        const uint8x16x4_t rgba = vld4q_u8(row + startX * 4);
        rLo = vmovl_u8(vget_low_u8(rgba.val[0]));
        rHi = vmovl_u8(vget_high_u8(rgba.val[0]));
        gLo = vmovl_u8(vget_low_u8(rgba.val[1]));
        gHi = vmovl_u8(vget_high_u8(rgba.val[1]));
        bLo = vmovl_u8(vget_low_u8(rgba.val[2]));
        bHi = vmovl_u8(vget_high_u8(rgba.val[2]));
        aLo = vmovl_u8(vget_low_u8(rgba.val[3]));
        aHi = vmovl_u8(vget_high_u8(rgba.val[3]));
      } else {
        // Wrapping — copy two runs into a contiguous buffer, then deinterleave.
        alignas(16) std::uint8_t pixBuf[64]; // 16 pixels × 4 bytes
        const std::uint32_t count1 = w - startX;
        const std::uint32_t count2 = 16 - count1;
        std::memcpy(pixBuf, row + startX * 4, count1 * 4);
        std::memcpy(pixBuf + count1 * 4, row, count2 * 4);
        const uint8x16x4_t rgba = vld4q_u8(pixBuf);
        rLo = vmovl_u8(vget_low_u8(rgba.val[0]));
        rHi = vmovl_u8(vget_high_u8(rgba.val[0]));
        gLo = vmovl_u8(vget_low_u8(rgba.val[1]));
        gHi = vmovl_u8(vget_high_u8(rgba.val[1]));
        bLo = vmovl_u8(vget_low_u8(rgba.val[2]));
        bHi = vmovl_u8(vget_high_u8(rgba.val[2]));
        aLo = vmovl_u8(vget_low_u8(rgba.val[3]));
        aHi = vmovl_u8(vget_high_u8(rgba.val[3]));
      }
    } else {
      // General nearest path: vectorized transform+tile, scattered pixel loads.
      const float dy05 = static_cast<float>(pipeline.dy) + 0.5f;
      const float txBase = dy05 * ctx.kx + ctx.tx;
      const float tyBase = dy05 * ctx.sy + ctx.ty;

      const float32x4_t vsx = vdupq_n_f32(ctx.sx);
      const float32x4_t vky = vdupq_n_f32(ctx.ky);
      const float32x4_t vtxBase = vdupq_n_f32(txBase);
      const float32x4_t vtyBase = vdupq_n_f32(tyBase);
      const float32x4_t vfw = vdupq_n_f32(fw);
      const float32x4_t vfh = vdupq_n_f32(fh);
      const float32x4_t vinvW = vdupq_n_f32(ctx.invWidth);
      const float32x4_t vinvH = vdupq_n_f32(ctx.invHeight);
      const float32x4_t vzero = vdupq_n_f32(0.0f);
      const float32x4_t vwMax = vdupq_n_f32(wMax);
      const float32x4_t vhMax = vdupq_n_f32(hMax);
      const uint32x4_t vPixW = vdupq_n_u32(w);

      alignas(16) std::uint16_t rArr[16], gArr[16], bArr[16], aArr[16];

      const float dx0 = static_cast<float>(pipeline.dx) + 0.5f;
      for (int group = 0; group < 4; ++group) {
        const float base = dx0 + static_cast<float>(group * 4);
        const float32x4_t px = {base, base + 1.0f, base + 2.0f, base + 3.0f};

        float32x4_t cx = vfmaq_f32(vtxBase, px, vsx);
        float32x4_t cy = vfmaq_f32(vtyBase, px, vky);

        if (ctx.spreadMode == SpreadMode::Repeat) {
          float32x4_t normX = vmulq_f32(cx, vinvW);
          float32x4_t normY = vmulq_f32(cy, vinvH);
          cx = vsubq_f32(cx, vmulq_f32(vrndmq_f32(normX), vfw));
          cy = vsubq_f32(cy, vmulq_f32(vrndmq_f32(normY), vfh));
        } else if (ctx.spreadMode == SpreadMode::Reflect) {
          // Scalar reflect per lane — vectorizing the reflect formula with
          // NEON floor+abs is possible but not worth it for correctness-first.
          alignas(16) float cxArr[4], cyArr[4];
          vst1q_f32(cxArr, cx);
          vst1q_f32(cyArr, cy);
          for (int lane = 0; lane < 4; ++lane) {
            cxArr[lane] = exclusiveReflect(cxArr[lane], fw, ctx.invWidth);
            cyArr[lane] = exclusiveReflect(cyArr[lane], fh, ctx.invHeight);
          }
          cx = vld1q_f32(cxArr);
          cy = vld1q_f32(cyArr);
        }

        cx = vmaxq_f32(cx, vzero);
        cx = vminq_f32(cx, vwMax);
        cy = vmaxq_f32(cy, vzero);
        cy = vminq_f32(cy, vhMax);

        const uint32x4_t ix = vcvtq_u32_f32(cx);
        const uint32x4_t iy = vcvtq_u32_f32(cy);
        const uint32x4_t idx = vmlaq_u32(ix, iy, vPixW);

        alignas(16) std::uint32_t indices[4];
        vst1q_u32(indices, idx);

        const int off = group * 4;
        for (int i = 0; i < 4; ++i) {
          std::uint32_t pixIdx = indices[i];
          if (pixIdx >= pixCount) pixIdx = pixCount - 1;
          const std::uint8_t* p = ctx.pixels + static_cast<std::size_t>(pixIdx) * 4;
          rArr[off + i] = p[0];
          gArr[off + i] = p[1];
          bArr[off + i] = p[2];
          aArr[off + i] = p[3];
        }
      }

      rLo = vld1q_u16(&rArr[0]); rHi = vld1q_u16(&rArr[8]);
      gLo = vld1q_u16(&gArr[0]); gHi = vld1q_u16(&gArr[8]);
      bLo = vld1q_u16(&bArr[0]); bHi = vld1q_u16(&bArr[8]);
      aLo = vld1q_u16(&aArr[0]); aHi = vld1q_u16(&aArr[8]);
    }

    if (ctx.opacity < 1.0f) {
      const std::uint16_t opac = static_cast<std::uint16_t>(ctx.opacity * 256.0f + 0.5f);
      const uint16x8_t vopac = vdupq_n_u16(opac);
      const uint16x8_t vhalf = vdupq_n_u16(128);
      auto scaleOpac = [&](uint16x8_t v) -> uint16x8_t {
        return vshrq_n_u16(vaddq_u16(vmulq_u16(v, vopac), vhalf), 8);
      };
      rLo = scaleOpac(rLo); rHi = scaleOpac(rHi);
      gLo = scaleOpac(gLo); gHi = scaleOpac(gHi);
      bLo = scaleOpac(bLo); bHi = scaleOpac(bHi);
      aLo = scaleOpac(aLo); aHi = scaleOpac(aHi);
    }

    // Fused SourceOver+Store: blend and write directly to pixmapDst.
    if (ctx.fuseSourceOver || ctx.fuseSourceOverCoverage) {
      // Scale by coverage if this is the AA edge pipeline.
      if (ctx.fuseSourceOverCoverage) {
        const uint16x8_t cov = vdupq_n_u16(
            static_cast<std::uint16_t>(pipeline.ctx->currentCoverage * 255.0f + 0.5f));
        const uint16x8_t v128 = vdupq_n_u16(128);
        auto scaleCov = [&](uint16x8_t v) -> uint16x8_t {
          return vshrq_n_u16(vaddq_u16(vmulq_u16(v, cov), v128), 8);
        };
        rLo = scaleCov(rLo); rHi = scaleCov(rHi);
        gLo = scaleCov(gLo); gHi = scaleCov(gHi);
        bLo = scaleCov(bLo); bHi = scaleCov(bHi);
        aLo = scaleCov(aLo); aHi = scaleCov(aHi);
      }

      auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
      if (pipeline.tail >= kStageWidth) {
        const auto* dstPacked = reinterpret_cast<const std::uint8_t*>(pixels.data());
        const uint8x16x4_t dstRgba = vld4q_u8(dstPacked);
        const uint16x8_t v128 = vdupq_n_u16(128);
        const uint16x8_t v255 = vdupq_n_u16(255);
        const uint16x8_t invALo = vsubq_u16(v255, aLo);
        const uint16x8_t invAHi = vsubq_u16(v255, aHi);
        auto blendCh = [&](uint16x8_t srcLo, uint16x8_t srcHi, uint8x16_t dst8) {
          uint16x8_t dLo = vmovl_u8(vget_low_u8(dst8));
          uint16x8_t dHi = vmovl_u8(vget_high_u8(dst8));
          uint16x8_t oLo = vaddq_u16(srcLo,
              vshrq_n_u16(vaddq_u16(vmulq_u16(dLo, invALo), v128), 8));
          uint16x8_t oHi = vaddq_u16(srcHi,
              vshrq_n_u16(vaddq_u16(vmulq_u16(dHi, invAHi), v128), 8));
          return vcombine_u8(vmovn_u16(oLo), vmovn_u16(oHi));
        };
        uint8x16x4_t outRgba;
        outRgba.val[0] = blendCh(rLo, rHi, dstRgba.val[0]);
        outRgba.val[1] = blendCh(gLo, gHi, dstRgba.val[1]);
        outRgba.val[2] = blendCh(bLo, bHi, dstRgba.val[2]);
        outRgba.val[3] = blendCh(aLo, aHi, dstRgba.val[3]);
        auto* outPacked = reinterpret_cast<std::uint8_t*>(pixels.data());
        vst4q_u8(outPacked, outRgba);
      } else {
        pipeline.r = U16x16T(rLo, rHi);
        pipeline.g = U16x16T(gLo, gHi);
        pipeline.b = U16x16T(bLo, bHi);
        pipeline.a = U16x16T(aLo, aHi);
        load8888Tail(pipeline.tail, pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
        pipeline.r = sourceOverChannel(pipeline.r, pipeline.dr, pipeline.a);
        pipeline.g = sourceOverChannel(pipeline.g, pipeline.dg, pipeline.a);
        pipeline.b = sourceOverChannel(pipeline.b, pipeline.db, pipeline.a);
        pipeline.a = sourceOverChannel(pipeline.a, pipeline.da, pipeline.a);
        store8888Tail(pipeline.tail, pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
      }
      pipeline.nextStage();
      return;
    }

    pipeline.r = U16x16T(rLo, rHi);
    pipeline.g = U16x16T(gLo, gHi);
    pipeline.b = U16x16T(bLo, bHi);
    pipeline.a = U16x16T(aLo, aHi);
#else
    // Generic scalar path: u8→u16 directly, no float intermediate.
    std::array<std::uint16_t, 16> rArr{}, gArr{}, bArr{}, aArr{};
    for (std::size_t i = 0; i < 16; ++i) {
      const float px = static_cast<float>(pipeline.dx + i) + 0.5f;
      const float py = static_cast<float>(pipeline.dy) + 0.5f;
      float cx = px * ctx.sx + py * ctx.kx + ctx.tx;
      float cy = px * ctx.ky + py * ctx.sy + ctx.ty;

      if (ctx.spreadMode == SpreadMode::Repeat) {
        cx = repeatTile(cx, fw, ctx.invWidth);
        cy = repeatTile(cy, fh, ctx.invHeight);
      } else if (ctx.spreadMode == SpreadMode::Reflect) {
        cx = exclusiveReflect(cx, fw, ctx.invWidth);
        cy = exclusiveReflect(cy, fh, ctx.invHeight);
      }
      cx = clampCoord(cx, wMax);
      cy = clampCoord(cy, hMax);

      std::uint32_t ix = static_cast<std::uint32_t>(static_cast<std::int32_t>(cy)) * w +
                         static_cast<std::uint32_t>(static_cast<std::int32_t>(cx));
      if (ix >= pixCount) ix = pixCount - 1;
      const std::uint8_t* p = ctx.pixels + static_cast<std::size_t>(ix) * 4;
      rArr[i] = p[0];
      gArr[i] = p[1];
      bArr[i] = p[2];
      aArr[i] = p[3];
    }

    if (ctx.opacity < 1.0f) {
      const std::uint16_t opac = static_cast<std::uint16_t>(ctx.opacity * 256.0f + 0.5f);
      for (std::size_t i = 0; i < 16; ++i) {
        rArr[i] = static_cast<std::uint16_t>((rArr[i] * opac + 128) >> 8);
        gArr[i] = static_cast<std::uint16_t>((gArr[i] * opac + 128) >> 8);
        bArr[i] = static_cast<std::uint16_t>((bArr[i] * opac + 128) >> 8);
        aArr[i] = static_cast<std::uint16_t>((aArr[i] * opac + 128) >> 8);
      }
    }

    // Fused SourceOver+Store for scalar nearest path.
    if (ctx.fuseSourceOver || ctx.fuseSourceOverCoverage) {
      if (ctx.fuseSourceOverCoverage) {
        const std::uint16_t cov =
            static_cast<std::uint16_t>(pipeline.ctx->currentCoverage * 255.0f + 0.5f);
        for (std::size_t i = 0; i < 16; ++i) {
          rArr[i] = static_cast<std::uint16_t>((rArr[i] * cov + 128) >> 8);
          gArr[i] = static_cast<std::uint16_t>((gArr[i] * cov + 128) >> 8);
          bArr[i] = static_cast<std::uint16_t>((bArr[i] * cov + 128) >> 8);
          aArr[i] = static_cast<std::uint16_t>((aArr[i] * cov + 128) >> 8);
        }
      }
      pipeline.r = U16x16T(rArr);
      pipeline.g = U16x16T(gArr);
      pipeline.b = U16x16T(bArr);
      pipeline.a = U16x16T(aArr);
      auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
      if (pipeline.tail >= kStageWidth) {
        load8888Lowp(pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
      } else {
        load8888Tail(pipeline.tail, pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
      }
      pipeline.r = sourceOverChannel(pipeline.r, pipeline.dr, pipeline.a);
      pipeline.g = sourceOverChannel(pipeline.g, pipeline.dg, pipeline.a);
      pipeline.b = sourceOverChannel(pipeline.b, pipeline.db, pipeline.a);
      pipeline.a = sourceOverChannel(pipeline.a, pipeline.da, pipeline.a);
      if (pipeline.tail >= kStageWidth) {
        store8888Lowp(pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
      } else {
        store8888Tail(pipeline.tail, pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
      }
      pipeline.nextStage();
      return;
    }

    pipeline.r = U16x16T(rArr);
    pipeline.g = U16x16T(gArr);
    pipeline.b = U16x16T(bArr);
    pipeline.a = U16x16T(aArr);
#endif
    pipeline.nextStage();
    return;
  }

  // Bilinear path: float-based for correct weighted interpolation.
  std::array<float, 16> rArr{}, gArr{}, bArr{}, aArr{};
  for (std::size_t i = 0; i < 16; ++i) {
    const float px = static_cast<float>(pipeline.dx + i) + 0.5f;
    const float py = static_cast<float>(pipeline.dy) + 0.5f;
    float cx = px * ctx.sx + py * ctx.kx + ctx.tx;
    float cy = px * ctx.ky + py * ctx.sy + ctx.ty;

    const float fx = (cx + 0.5f) - std::floor(cx + 0.5f);
    const float fy = (cy + 0.5f) - std::floor(cy + 0.5f);
    const float wx0 = 1.0f - fx, wx1 = fx;
    const float wy0 = 1.0f - fy, wy1 = fy;

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float sy = cy - 0.5f;
    for (int j = 0; j < 2; ++j) {
      float sx = cx - 0.5f;
      for (int k = 0; k < 2; ++k) {
        float tx = sx, ty = sy;
        if (ctx.spreadMode == SpreadMode::Repeat) {
          tx = repeatTile(tx, fw, ctx.invWidth);
          ty = repeatTile(ty, fh, ctx.invHeight);
        } else if (ctx.spreadMode == SpreadMode::Reflect) {
          tx = exclusiveReflect(tx, fw, ctx.invWidth);
          ty = exclusiveReflect(ty, fh, ctx.invHeight);
        }
        tx = clampCoord(tx, wMax);
        ty = clampCoord(ty, hMax);
        std::uint32_t ix = static_cast<std::uint32_t>(static_cast<std::int32_t>(ty)) * w +
                           static_cast<std::uint32_t>(static_cast<std::int32_t>(tx));
        if (ix >= pixCount) ix = pixCount - 1;

        float sr, sg, sb, sa;
        loadPixelRGBA(ctx.pixels, ix, sr, sg, sb, sa);
        const float wt = (k == 0 ? wx0 : wx1) * (j == 0 ? wy0 : wy1);
        r += wt * sr;
        g += wt * sg;
        b += wt * sb;
        a += wt * sa;
        sx += 1.0f;
      }
      sy += 1.0f;
    }

    if (ctx.opacity < 1.0f) {
      r *= ctx.opacity;
      g *= ctx.opacity;
      b *= ctx.opacity;
      a *= ctx.opacity;
    }

    rArr[i] = r;
    gArr[i] = g;
    bArr[i] = b;
    aArr[i] = a;
  }

  auto rf = F32x16T(F32x8T({rArr[0], rArr[1], rArr[2], rArr[3], rArr[4], rArr[5], rArr[6], rArr[7]}),
                    F32x8T({rArr[8], rArr[9], rArr[10], rArr[11], rArr[12], rArr[13], rArr[14], rArr[15]}));
  auto gf = F32x16T(F32x8T({gArr[0], gArr[1], gArr[2], gArr[3], gArr[4], gArr[5], gArr[6], gArr[7]}),
                    F32x8T({gArr[8], gArr[9], gArr[10], gArr[11], gArr[12], gArr[13], gArr[14], gArr[15]}));
  auto bf = F32x16T(F32x8T({bArr[0], bArr[1], bArr[2], bArr[3], bArr[4], bArr[5], bArr[6], bArr[7]}),
                    F32x8T({bArr[8], bArr[9], bArr[10], bArr[11], bArr[12], bArr[13], bArr[14], bArr[15]}));
  auto af = F32x16T(F32x8T({aArr[0], aArr[1], aArr[2], aArr[3], aArr[4], aArr[5], aArr[6], aArr[7]}),
                    F32x8T({aArr[8], aArr[9], aArr[10], aArr[11], aArr[12], aArr[13], aArr[14], aArr[15]}));
  roundF32ToU16(rf, gf, bf, af, pipeline.r, pipeline.g, pipeline.b, pipeline.a);

  // Fused SourceOver+Store for bilinear path.
  if (ctx.fuseSourceOver || ctx.fuseSourceOverCoverage) {
    if (ctx.fuseSourceOverCoverage) {
      const auto cov = fromFloat(pipeline.ctx->currentCoverage);
      pipeline.r = mulDiv255(pipeline.r, cov);
      pipeline.g = mulDiv255(pipeline.g, cov);
      pipeline.b = mulDiv255(pipeline.b, cov);
      pipeline.a = mulDiv255(pipeline.a, cov);
    }
    auto pixels = pixelsAtXY(*pipeline.pixmapDst, pipeline.dx, pipeline.dy);
    if (pipeline.tail >= kStageWidth) {
      load8888Lowp(pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
    } else {
      load8888Tail(pipeline.tail, pixels, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
    }
    pipeline.r = sourceOverChannel(pipeline.r, pipeline.dr, pipeline.a);
    pipeline.g = sourceOverChannel(pipeline.g, pipeline.dg, pipeline.a);
    pipeline.b = sourceOverChannel(pipeline.b, pipeline.db, pipeline.a);
    pipeline.a = sourceOverChannel(pipeline.a, pipeline.da, pipeline.a);
    if (pipeline.tail >= kStageWidth) {
      store8888Lowp(pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
    } else {
      store8888Tail(pipeline.tail, pixels, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
    }
    pipeline.nextStage();
    return;
  }

  pipeline.nextStage();
}

}  // namespace

void justReturn(Pipeline& pipeline) { (void)pipeline; }

void start(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& functions,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& tailFunctions,
           const ScreenIntRect& rect, const AAMaskCtx& aaMaskCtx, const MaskCtx& maskCtx,
           Context& ctx, MutableSubPixmapView* pixmapDst) {
  Pipeline p(functions, tailFunctions, rect, aaMaskCtx, maskCtx, ctx, pixmapDst);

  for (std::size_t y = rect.y(); y < rect.bottom(); ++y) {
    std::size_t x = rect.x();
    const std::size_t end = rect.right();

    p.functions = &functions;
    p.scanlineDone = false;
    while (x + kStageWidth <= end) {
      p.index = 0;
      p.dx = x;
      p.dy = y;
      p.tail = kStageWidth;
      p.nextStage();
      if (p.scanlineDone) break;
      x += kStageWidth;
    }

    if (!p.scanlineDone && x != end) {
      p.index = 0;
      p.functions = &tailFunctions;
      p.dx = x;
      p.dy = y;
      p.tail = end - x;
      p.nextStage();
    }
  }
}

const std::array<StageFn, kStagesCount> STAGES = {
    moveSourceToDestination,
    moveDestinationToSource,
    nullFn,
    nullFn,
    premultiply,
    uniformColor,
    seedShader,
    loadDst,
    store,
    loadDstU8,
    storeU8,
    nullFn,
    loadMaskU8,
    maskU8,
    scaleU8,
    lerpU8,
    scale1Float,
    lerp1Float,
    destinationAtop,
    destinationIn,
    destinationOut,
    destinationOver,
    sourceAtop,
    sourceIn,
    sourceOut,
    sourceOver,
    clear,
    modulate,
    multiply,
    plus,
    screen,
    xOr,
    nullFn,
    nullFn,
    darken,
    difference,
    exclusion,
    hardLight,
    lighten,
    overlay,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,  // Luminosity (not lowp compatible)
    sourceOverRgba,
    transform,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    padX1,
    reflectX1,
    repeatX1,
    gradient,
    evenlySpaced2StopGradient,
    nullFn,
    xyToRadius,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,
    nullFn,  // Unpremultiply (highp only)
    nullFn,  // PremultiplyDestination (highp only)
    fusedLinearGradient2Stop,
    fusedRadialGradient2Stop,
    fusedBilinearPattern,
};

const std::array<StageFn, kStagesCount> STAGES_TAIL = [] {
  auto stages = STAGES;
  stages[static_cast<std::size_t>(Stage::LoadDestination)] = loadDstTail;
  stages[static_cast<std::size_t>(Stage::Store)] = storeTail;
  stages[static_cast<std::size_t>(Stage::LoadDestinationU8)] = loadDstU8Tail;
  stages[static_cast<std::size_t>(Stage::StoreU8)] = storeU8Tail;
  stages[static_cast<std::size_t>(Stage::SourceOverRgba)] = sourceOverRgbaTail;
  return stages;
}();

}  // namespace tiny_skia::pipeline::lowp
