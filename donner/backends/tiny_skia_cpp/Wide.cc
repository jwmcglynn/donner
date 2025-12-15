#include "donner/backends/tiny_skia_cpp/Wide.h"

#include <cmath>

#define DONNER_TINY_SKIA_ENABLE_WIDE_SIMD 0

#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__) && DONNER_TINY_SKIA_ENABLE_WIDE_SIMD
#include <emmintrin.h>
#define DONNER_TINY_SKIA_USE_SSE2 1
#endif

#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && (defined(__ARM_NEON) || defined(__ARM_NEON__)) && \
    DONNER_TINY_SKIA_ENABLE_WIDE_SIMD
#include <arm_neon.h>
#define DONNER_TINY_SKIA_USE_NEON 1
#endif

namespace donner::backends::tiny_skia_cpp {

namespace {

constexpr int kLaneCount = 4;

}  // namespace

F32x4::F32x4()
    : values_{} {}

F32x4 F32x4::Splat(float value) {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t splat = vdupq_n_f32(value);
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, splat);
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 splat = _mm_set1_ps(value);
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, splat);
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#else
  return F32x4(std::array<float, kLaneCount>{value, value, value, value});
#endif
}

F32x4 F32x4::FromArray(const std::array<float, 4>& values) {
  return F32x4(values);
}

F32x4 F32x4::FromColor(const Color& color) {
  return FromArray({static_cast<float>(color.r), static_cast<float>(color.g),
                    static_cast<float>(color.b), static_cast<float>(color.a)});
}

std::array<float, 4> F32x4::toArray() const {
  return values_;
}

F32x4 F32x4::operator+(const F32x4& rhs) const {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t lhsVec = vld1q_f32(values_.data());
  const float32x4_t rhsVec = vld1q_f32(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, vaddq_f32(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 lhsVec = _mm_loadu_ps(values_.data());
  const __m128 rhsVec = _mm_loadu_ps(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, _mm_add_ps(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#else
  std::array<float, kLaneCount> out;
  for (int i = 0; i < kLaneCount; ++i) {
    out[i] = values_[i] + rhs.values_[i];
  }
  return F32x4(out);
#endif
}

F32x4 F32x4::operator-(const F32x4& rhs) const {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t lhsVec = vld1q_f32(values_.data());
  const float32x4_t rhsVec = vld1q_f32(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, vsubq_f32(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 lhsVec = _mm_loadu_ps(values_.data());
  const __m128 rhsVec = _mm_loadu_ps(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, _mm_sub_ps(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#else
  std::array<float, kLaneCount> out;
  for (int i = 0; i < kLaneCount; ++i) {
    out[i] = values_[i] - rhs.values_[i];
  }
  return F32x4(out);
#endif
}

F32x4 F32x4::operator*(const F32x4& rhs) const {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t lhsVec = vld1q_f32(values_.data());
  const float32x4_t rhsVec = vld1q_f32(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, vmulq_f32(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 lhsVec = _mm_loadu_ps(values_.data());
  const __m128 rhsVec = _mm_loadu_ps(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, _mm_mul_ps(lhsVec, rhsVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#else
  std::array<float, kLaneCount> out;
  for (int i = 0; i < kLaneCount; ++i) {
    out[i] = values_[i] * rhs.values_[i];
  }
  return F32x4(out);
#endif
}

F32x4 F32x4::operator*(float scalar) const {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t lhsVec = vld1q_f32(values_.data());
  const float32x4_t scalarVec = vdupq_n_f32(scalar);
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, vmulq_f32(lhsVec, scalarVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 lhsVec = _mm_loadu_ps(values_.data());
  const __m128 scalarVec = _mm_set1_ps(scalar);
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, _mm_mul_ps(lhsVec, scalarVec));
  return F32x4(std::array<float, kLaneCount>{values[0], values[1], values[2], values[3]});
#else
  std::array<float, kLaneCount> out;
  for (int i = 0; i < kLaneCount; ++i) {
    out[i] = values_[i] * scalar;
  }
  return F32x4(out);
#endif
}

F32x4 F32x4::operator/(float scalar) const {
  const float inv = scalar == 0.0f ? 0.0f : 1.0f / scalar;
  return (*this) * inv;
}

F32x4& F32x4::operator+=(const F32x4& rhs) {
#if defined(DONNER_TINY_SKIA_USE_NEON)
  const float32x4_t lhsVec = vld1q_f32(values_.data());
  const float32x4_t rhsVec = vld1q_f32(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  vst1q_f32(values, vaddq_f32(lhsVec, rhsVec));
  values_ = {values[0], values[1], values[2], values[3]};
  return *this;
#elif defined(DONNER_TINY_SKIA_USE_SSE2)
  const __m128 lhsVec = _mm_loadu_ps(values_.data());
  const __m128 rhsVec = _mm_loadu_ps(rhs.values_.data());
  alignas(16) float values[kLaneCount];
  _mm_storeu_ps(values, _mm_add_ps(lhsVec, rhsVec));
  values_ = {values[0], values[1], values[2], values[3]};
  return *this;
#else
  for (int i = 0; i < kLaneCount; ++i) {
    values_[i] += rhs.values_[i];
  }
  return *this;
#endif
}
F32x4::F32x4(const std::array<float, 4>& values) : values_(values) {}

}  // namespace donner::backends::tiny_skia_cpp

