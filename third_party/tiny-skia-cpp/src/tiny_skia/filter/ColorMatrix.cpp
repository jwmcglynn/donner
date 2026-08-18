#include "tiny_skia/filter/ColorMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// Selects the ISA branch below and pulls in the matching intrinsics header.
#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

/// The 5x4 color matrix rearranged into one vector per input component, so the
/// per-pixel work multiplies and accumulates all four output channels at once.
///
/// Built once per `colorMatrix()` call; `apply()` runs per pixel.
///
/// The wasm128, SSE2, and scalar branches all evaluate the same products and
/// sums in the same left-to-right order, so their results are bit-identical.
/// The NEON branch is not: it fuses each multiply and add into a single
/// rounding step and accumulates the translation column first, which can differ
/// by one unit in the last place. That predates the other branches and changing
/// it would move rendered output on ARM.
class ColorMatrixColumns {
 public:
#if defined(TINY_SKIA_SIMD_NEON)
  explicit ColorMatrixColumns(const float (&m)[20]) {
    // Each column holds one input component's coefficients across all four
    // outputs.
    const float32x4_t colR = {m[0], m[5], m[10], m[15]};
    const float32x4_t colG = {m[1], m[6], m[11], m[16]};
    const float32x4_t colB = {m[2], m[7], m[12], m[17]};
    const float32x4_t colA = {m[3], m[8], m[13], m[18]};
    const float32x4_t col1 = {m[4], m[9], m[14], m[19]};
    colR_ = colR;
    colG_ = colG;
    colB_ = colB;
    colA_ = colA;
    col1_ = col1;
  }

  void apply(float r, float g, float b, float pa, float* out) const {
    // result = col_1 + col_r*r + col_g*g + col_b*b + col_a*pa
    float32x4_t result = col1_;
    result = vfmaq_n_f32(result, colR_, r);
    result = vfmaq_n_f32(result, colG_, g);
    result = vfmaq_n_f32(result, colB_, b);
    result = vfmaq_n_f32(result, colA_, pa);

    // Clamp all channels to [0, 1].
    result = vmaxq_f32(result, vdupq_n_f32(0.0f));
    result = vminq_f32(result, vdupq_n_f32(1.0f));

    // Extract the new alpha and premultiply RGB by it.
    const float ca = vgetq_lane_f32(result, 3);
    vst1q_f32(out, vmulq_n_f32(result, ca));
    out[3] = ca;
  }

 private:
  float32x4_t colR_;
  float32x4_t colG_;
  float32x4_t colB_;
  float32x4_t colA_;
  float32x4_t col1_;

#elif defined(TINY_SKIA_SIMD_WASM_SIMD128)
  explicit ColorMatrixColumns(const float (&m)[20])
      : colR_(wasm_f32x4_make(m[0], m[5], m[10], m[15])),
        colG_(wasm_f32x4_make(m[1], m[6], m[11], m[16])),
        colB_(wasm_f32x4_make(m[2], m[7], m[12], m[17])),
        colA_(wasm_f32x4_make(m[3], m[8], m[13], m[18])),
        col1_(wasm_f32x4_make(m[4], m[9], m[14], m[19])) {}

  void apply(float r, float g, float b, float pa, float* out) const {
    // Accumulated in the scalar fallback's order with a separate multiply and
    // add per term. wasm128 has no fused multiply-add outside relaxed SIMD, so
    // each lane rounds exactly where the scalar expression rounds.
    v128_t acc = wasm_f32x4_mul(colR_, wasm_f32x4_splat(r));
    acc = wasm_f32x4_add(acc, wasm_f32x4_mul(colG_, wasm_f32x4_splat(g)));
    acc = wasm_f32x4_add(acc, wasm_f32x4_mul(colB_, wasm_f32x4_splat(b)));
    acc = wasm_f32x4_add(acc, wasm_f32x4_mul(colA_, wasm_f32x4_splat(pa)));
    acc = wasm_f32x4_add(acc, col1_);

    // pmin and pmax are plain lane selects, pmin(x, y) = y < x ? y : x and
    // pmax(x, y) = x < y ? y : x, so this operand order evaluates
    // std::clamp's `v < lo ? lo : (hi < v ? hi : v)` exactly. The IEEE
    // f32x4.min/max would order the operands the other way round.
    const v128_t clamped =
        wasm_f32x4_pmin(wasm_f32x4_pmax(acc, wasm_f32x4_splat(0.0f)), wasm_f32x4_splat(1.0f));

    // The scalar fallback clamps the premultiplied channels a second time.
    // That clamp is an identity here: both factors are already in [0, 1] (or
    // NaN, which every clamp passes through), so their product is too.
    const float ca = wasm_f32x4_extract_lane(clamped, 3);
    wasm_v128_store(out, wasm_f32x4_mul(clamped, wasm_f32x4_splat(ca)));
    out[3] = ca;
  }

 private:
  v128_t colR_;
  v128_t colG_;
  v128_t colB_;
  v128_t colA_;
  v128_t col1_;

#elif defined(TINY_SKIA_SIMD_SSE2)
  explicit ColorMatrixColumns(const float (&m)[20])
      : colR_(_mm_setr_ps(m[0], m[5], m[10], m[15])),
        colG_(_mm_setr_ps(m[1], m[6], m[11], m[16])),
        colB_(_mm_setr_ps(m[2], m[7], m[12], m[17])),
        colA_(_mm_setr_ps(m[3], m[8], m[13], m[18])),
        col1_(_mm_setr_ps(m[4], m[9], m[14], m[19])) {}

  void apply(float r, float g, float b, float pa, float* out) const {
    // Accumulated in the scalar fallback's order with a separate multiply and
    // add per term, so each lane rounds exactly where the scalar expression
    // rounds. The library builds with -ffp-contract=off, so the compiler does
    // not fuse these into FMAs either.
    __m128 acc = _mm_mul_ps(colR_, _mm_set1_ps(r));
    acc = _mm_add_ps(acc, _mm_mul_ps(colG_, _mm_set1_ps(g)));
    acc = _mm_add_ps(acc, _mm_mul_ps(colB_, _mm_set1_ps(b)));
    acc = _mm_add_ps(acc, _mm_mul_ps(colA_, _mm_set1_ps(pa)));
    acc = _mm_add_ps(acc, col1_);

    // _mm_max_ps(a, b) selects `a > b ? a : b` and _mm_min_ps(a, b) selects
    // `a < b ? a : b`, so naming the bound first evaluates std::clamp's
    // `v < lo ? lo : (hi < v ? hi : v)` exactly.
    const __m128 clamped = _mm_min_ps(_mm_set1_ps(1.0f), _mm_max_ps(_mm_setzero_ps(), acc));

    // As in the wasm128 branch, the scalar fallback's second clamp of the
    // premultiplied channels is an identity and is dropped.
    const float ca = _mm_cvtss_f32(_mm_shuffle_ps(clamped, clamped, _MM_SHUFFLE(3, 3, 3, 3)));
    _mm_storeu_ps(out, _mm_mul_ps(clamped, _mm_set1_ps(ca)));
    out[3] = ca;
  }

 private:
  __m128 colR_;
  __m128 colG_;
  __m128 colB_;
  __m128 colA_;
  __m128 col1_;

#else
  explicit ColorMatrixColumns(const float (&m)[20]) {
    for (int j = 0; j < 20; ++j) {
      m_[j] = m[j];
    }
  }

  void apply(float r, float g, float b, float pa, float* out) const {
    // Apply the 5x4 matrix.
    const float nr = m_[0] * r + m_[1] * g + m_[2] * b + m_[3] * pa + m_[4];
    const float ng = m_[5] * r + m_[6] * g + m_[7] * b + m_[8] * pa + m_[9];
    const float nb = m_[10] * r + m_[11] * g + m_[12] * b + m_[13] * pa + m_[14];
    const float na = m_[15] * r + m_[16] * g + m_[17] * b + m_[18] * pa + m_[19];

    // Clamp and re-premultiply.
    const float ca = std::clamp(na, 0.0f, 1.0f);
    out[0] = std::clamp(std::clamp(nr, 0.0f, 1.0f) * ca, 0.0f, 1.0f);
    out[1] = std::clamp(std::clamp(ng, 0.0f, 1.0f) * ca, 0.0f, 1.0f);
    out[2] = std::clamp(std::clamp(nb, 0.0f, 1.0f) * ca, 0.0f, 1.0f);
    out[3] = ca;
  }

 private:
  float m_[20];
#endif
};

}  // namespace

void colorMatrix(Pixmap& pixmap, const std::array<double, 20>& matrix) {
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t offset = i * 4;
    const double pa = data[offset + 3];

    if (pa == 0) {
      // Fully transparent: only the translation components can produce non-zero output.
      // Apply matrix to [0,0,0,0,1].
      const double nr = matrix[4] * 255.0;
      const double ng = matrix[9] * 255.0;
      const double nb = matrix[14] * 255.0;
      const double na = matrix[19] * 255.0;

      const double ca = std::clamp(na, 0.0, 255.0);
      if (ca == 0) {
        continue;  // Still transparent.
      }
      const double alphaScale = ca / 255.0;
      data[offset + 0] =
          static_cast<std::uint8_t>(std::clamp(std::round(nr * alphaScale), 0.0, 255.0));
      data[offset + 1] =
          static_cast<std::uint8_t>(std::clamp(std::round(ng * alphaScale), 0.0, 255.0));
      data[offset + 2] =
          static_cast<std::uint8_t>(std::clamp(std::round(nb * alphaScale), 0.0, 255.0));
      data[offset + 3] = static_cast<std::uint8_t>(std::round(ca));
      continue;
    }

    // Unpremultiply.
    const double invAlpha = 255.0 / pa;
    const double r = data[offset + 0] * invAlpha;
    const double g = data[offset + 1] * invAlpha;
    const double b = data[offset + 2] * invAlpha;
    const double a = pa;

    // Apply 5x4 matrix: [R,G,B,A,1] -> [R',G',B',A']
    // Translation components (matrix[4], [9], [14], [19]) are in 0-1 range per SVG spec,
    // scaled to 0-255 here.
    const double nr = matrix[0] * r + matrix[1] * g + matrix[2] * b + matrix[3] * a +
                      matrix[4] * 255.0;
    const double ng = matrix[5] * r + matrix[6] * g + matrix[7] * b + matrix[8] * a +
                      matrix[9] * 255.0;
    const double nb = matrix[10] * r + matrix[11] * g + matrix[12] * b + matrix[13] * a +
                      matrix[14] * 255.0;
    const double na = matrix[15] * r + matrix[16] * g + matrix[17] * b + matrix[18] * a +
                      matrix[19] * 255.0;

    // Clamp and re-premultiply.
    const double ca = std::clamp(na, 0.0, 255.0);
    const double alphaScale = ca / 255.0;
    data[offset + 0] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(nr, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 1] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(ng, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 2] =
        static_cast<std::uint8_t>(std::clamp(std::round(std::clamp(nb, 0.0, 255.0) * alphaScale), 0.0, 255.0));
    data[offset + 3] = static_cast<std::uint8_t>(std::round(ca));
  }
}

void colorMatrix(FloatPixmap& pixmap, const std::array<double, 20>& matrix) {
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;
  float* ptr = data.data();

  // Pre-convert matrix to float for faster per-pixel math.
  float m[20];
  for (int j = 0; j < 20; ++j) {
    m[j] = static_cast<float>(matrix[j]);
  }

  const ColorMatrixColumns columns(m);

  for (std::size_t i = 0; i < pixelCount; ++i) {
    float* px = ptr + i * 4;
    const float pa = px[3];

    if (pa == 0.0f) {
      // Fully transparent: only the translation column can produce output.
      const float ca = std::clamp(m[19], 0.0f, 1.0f);
      if (ca == 0.0f) {
        continue;
      }
      px[0] = std::clamp(m[4] * ca, 0.0f, 1.0f);
      px[1] = std::clamp(m[9] * ca, 0.0f, 1.0f);
      px[2] = std::clamp(m[14] * ca, 0.0f, 1.0f);
      px[3] = ca;
      continue;
    }

    // Unpremultiply, then apply the matrix, clamp, and re-premultiply.
    const float invAlpha = 1.0f / pa;
    columns.apply(px[0] * invAlpha, px[1] * invAlpha, px[2] * invAlpha, pa, px);
  }
}

std::array<double, 20> saturateMatrix(double s) {
  // clang-format off
  return {
    0.2126 + 0.7874 * s, 0.7152 - 0.7152 * s, 0.0722 - 0.0722 * s, 0, 0,
    0.2126 - 0.2126 * s, 0.7152 + 0.2848 * s, 0.0722 - 0.0722 * s, 0, 0,
    0.2126 - 0.2126 * s, 0.7152 - 0.7152 * s, 0.0722 + 0.9278 * s, 0, 0,
    0,                   0,                    0,                    1, 0,
  };
  // clang-format on
}

std::array<double, 20> hueRotateMatrix(double angleDeg) {
  const double rad = angleDeg * M_PI / 180.0;
  const double cosA = std::cos(rad);
  const double sinA = std::sin(rad);
  // clang-format off
  return {
    0.213 + cosA * 0.787 - sinA * 0.213,
    0.715 - cosA * 0.715 - sinA * 0.715,
    0.072 - cosA * 0.072 + sinA * 0.928,
    0, 0,
    0.213 - cosA * 0.213 + sinA * 0.143,
    0.715 + cosA * 0.285 + sinA * 0.140,
    0.072 - cosA * 0.072 - sinA * 0.283,
    0, 0,
    0.213 - cosA * 0.213 - sinA * 0.787,
    0.715 - cosA * 0.715 + sinA * 0.715,
    0.072 + cosA * 0.928 + sinA * 0.072,
    0, 0,
    0, 0, 0, 1, 0,
  };
  // clang-format on
}

std::array<double, 20> luminanceToAlphaMatrix() {
  // clang-format off
  return {
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    0.2126, 0.7152, 0.0722, 0, 0,
  };
  // clang-format on
}

std::array<double, 20> identityMatrix() {
  // clang-format off
  return {
    1, 0, 0, 0, 0,
    0, 1, 0, 0, 0,
    0, 0, 1, 0, 0,
    0, 0, 0, 1, 0,
  };
  // clang-format on
}

}  // namespace tiny_skia::filter
