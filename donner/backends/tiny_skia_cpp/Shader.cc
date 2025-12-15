#include "donner/backends/tiny_skia_cpp/Shader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_AVX2) && defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__)
#include <emmintrin.h>
#endif

#define DONNER_TINY_SKIA_ENABLE_SHADER_SIMD 0

#include "donner/backends/tiny_skia_cpp/CpuFeatures.h"
#include "donner/backends/tiny_skia_cpp/Wide.h"
#include "donner/base/MathUtils.h"

namespace donner::backends::tiny_skia_cpp {

namespace {

constexpr float kDegenerateThreshold = 1.0f / static_cast<float>(1 << 15);

Color LerpColors(const Color& left, const Color& right, float t);

float ClampToUnit(float value) {
  return std::clamp(value, 0.0f, 1.0f);
}

size_t FindInitialStopIndex(const GradientData& gradient, float position) {
  size_t index = 0;
  for (size_t i = 1; i < gradient.stops.size(); ++i) {
    if (position < gradient.stops[i].position || NearEquals(position, gradient.stops[i].position)) {
      index = i - 1;
      break;
    }
  }
  return index;
}

Color SampleGradientWithIndex(const GradientData& gradient, float t, float step,
                              size_t& stopIndex) {
  const float clamped = ClampToUnit(t);

  if (step >= 0.0f) {
    while (stopIndex + 1 < gradient.stops.size() &&
           (clamped > gradient.stops[stopIndex + 1].position &&
            !NearEquals(clamped, gradient.stops[stopIndex + 1].position))) {
      ++stopIndex;
    }
  } else {
    while (stopIndex > 0 && (clamped < gradient.stops[stopIndex].position &&
                             !NearEquals(clamped, gradient.stops[stopIndex].position))) {
      --stopIndex;
    }
  }

  const size_t rightIndex = std::min(stopIndex + 1, gradient.stops.size() - 1);
  const GradientStop& left = gradient.stops[stopIndex];
  const GradientStop& right = gradient.stops[rightIndex];

  const float span = right.position - left.position;
  if (NearZero(span)) {
    return right.color;
  }

  const float ratio = (clamped - left.position) / span;
  return LerpColors(left.color, right.color, ratio);
}

uint8_t ClampToByte(float value) {
  const float clamped = std::clamp(value, 0.0f, 255.0f);
  return static_cast<uint8_t>(std::lround(clamped));
}

Color MultiplyColor(Color color, float scale) {
  const float clamped = std::clamp(scale, 0.0f, 1.0f);
  const auto scaleChannel = [clamped](uint8_t channel) {
    return static_cast<uint8_t>(std::lround(clamped * static_cast<float>(channel)));
  };

  color.r = scaleChannel(color.r);
  color.g = scaleChannel(color.g);
  color.b = scaleChannel(color.b);
  color.a = scaleChannel(color.a);
  return color;
}

Color LerpColors(const Color& left, const Color& right, float t) {
  const float clamped = ClampToUnit(t);
  const float invT = 1.0f - clamped;

  const F32x4 blended = F32x4::FromColor(left) * invT + F32x4::FromColor(right) * clamped;
  const std::array<float, 4> channels = blended.toArray();
  return Color{ClampToByte(channels[0]), ClampToByte(channels[1]), ClampToByte(channels[2]),
               ClampToByte(channels[3])};
}

bool IsTransformInvertible(const Transform& transform) {
  const double det = transform.determinant();
  return !NearZero(det, std::numeric_limits<double>::epsilon());
}

Color AverageColor(const std::vector<GradientStop>& stops) {
  if (stops.empty()) {
    return {};
  }

  F32x4 accum = F32x4::Splat(0.0f);
  for (const GradientStop& stop : stops) {
    accum += F32x4::FromColor(stop.color);
  }

  const float invCount = 1.0f / static_cast<float>(stops.size());
  const std::array<float, 4> averaged = (accum * invCount).toArray();
  return Color{ClampToByte(averaged[0]), ClampToByte(averaged[1]), ClampToByte(averaged[2]),
               ClampToByte(averaged[3])};
}

}  // namespace

Shader Shader::MakeSolidColor(Color color) {
  return Shader(SolidColorShader{color});
}

Expected<Shader, std::string> Shader::MakeLinearGradient(Vector2d start, Vector2d end,
                                                         std::vector<GradientStop> stops,
                                                         SpreadMode spread,
                                                         const Transform& transform) {
  if (stops.empty()) {
    return Expected<Shader, std::string>::Failure("linear gradient requires at least one stop");
  }

  if (stops.size() == 1) {
    return Expected<Shader, std::string>::Success(Shader::MakeSolidColor(stops.front().color));
  }

  if (!IsTransformInvertible(transform)) {
    return Expected<Shader, std::string>::Failure("linear gradient transform is not invertible");
  }

  const bool isDegenerate = start == end;
  if (isDegenerate) {
    switch (spread) {
      case SpreadMode::kPad:
        return Expected<Shader, std::string>::Success(Shader::MakeSolidColor(stops.back().color));
      case SpreadMode::kRepeat:
      case SpreadMode::kReflect:
        return Expected<Shader, std::string>::Success(Shader::MakeSolidColor(AverageColor(stops)));
    }
  }

  Expected<GradientData, std::string> normalizedStops = NormalizeStops(std::move(stops));
  if (!normalizedStops.hasValue()) {
    return Expected<Shader, std::string>::Failure(normalizedStops.error());
  }

  LinearGradientShader shader{start, end, spread, transform, std::move(normalizedStops.value())};
  return Expected<Shader, std::string>::Success(Shader(shader));
}

Expected<Shader, std::string> Shader::MakeRadialGradient(Vector2d start, Vector2d end, float radius,
                                                         std::vector<GradientStop> stops,
                                                         SpreadMode spread,
                                                         const Transform& transform) {
  if (radius < 0.0f || NearZero(radius)) {
    return Expected<Shader, std::string>::Failure("radial gradient requires a positive radius");
  }

  if (stops.empty()) {
    return Expected<Shader, std::string>::Failure("radial gradient requires at least one stop");
  }

  if (stops.size() == 1) {
    return Expected<Shader, std::string>::Success(Shader::MakeSolidColor(stops.front().color));
  }

  if (!IsTransformInvertible(transform)) {
    return Expected<Shader, std::string>::Failure("radial gradient transform is not invertible");
  }

  const Vector2d delta = end - start;
  const double length = delta.length();
  if (!std::isfinite(length)) {
    return Expected<Shader, std::string>::Failure("radial gradient has a non-finite center delta");
  }

  const bool coincidentCenters = NearZero(length, static_cast<double>(kDegenerateThreshold));
  if (coincidentCenters && NearZero(radius, kDegenerateThreshold)) {
    return Expected<Shader, std::string>::Failure("radial gradient is fully degenerate");
  }

  Expected<GradientData, std::string> normalizedStops = NormalizeStops(std::move(stops));
  if (!normalizedStops.hasValue()) {
    return Expected<Shader, std::string>::Failure(normalizedStops.error());
  }

  RadialGradientShader shader{start,  end,       radius,
                              spread, transform, std::move(normalizedStops.value())};
  return Expected<Shader, std::string>::Success(Shader(shader));
}

Expected<Shader, std::string> Shader::MakePattern(const Pixmap& pixmap, SpreadMode spread,
                                                  FilterQuality quality, float opacity,
                                                  const Transform& transform) {
  if (!pixmap.isValid()) {
    return Expected<Shader, std::string>::Failure("pattern pixmap is invalid");
  }

  if (!IsTransformInvertible(transform)) {
    return Expected<Shader, std::string>::Failure("pattern transform is not invertible");
  }

  PatternShader shader{&pixmap, spread, quality, std::clamp(opacity, 0.0f, 1.0f), transform};
  return Expected<Shader, std::string>::Success(Shader(shader));
}

Shader::Shader(SolidColorShader shader)
    : kind_(ShaderKind::kSolidColor), data_(std::move(shader)) {}

Shader::Shader(LinearGradientShader shader)
    : kind_(ShaderKind::kLinearGradient), data_(std::move(shader)) {}

Shader::Shader(RadialGradientShader shader)
    : kind_(ShaderKind::kRadialGradient), data_(std::move(shader)) {}

Shader::Shader(PatternShader shader) : kind_(ShaderKind::kPattern), data_(std::move(shader)) {}

Expected<GradientData, std::string> Shader::NormalizeStops(std::vector<GradientStop> stops) {
  if (stops.size() < 2) {
    return Expected<GradientData, std::string>::Failure("gradient requires at least two stops");
  }

  const bool dummyFirst = !NearZero(stops.front().position);
  const bool dummyLast = !NearEquals(stops.back().position, 1.0f);

  if (dummyFirst) {
    stops.insert(stops.begin(), GradientStop{0.0f, stops.front().color});
  } else {
    stops.front().position = 0.0f;
  }

  if (dummyLast) {
    stops.push_back(GradientStop{1.0f, stops.back().color});
  } else {
    stops.back().position = 1.0f;
  }

  bool colorsAreOpaque = true;
  for (const GradientStop& stop : stops) {
    colorsAreOpaque &= stop.color.a == 0xFF;
  }

  const size_t startIndex = dummyFirst ? 0 : 1;
  float prev = 0.0f;
  bool hasUniformStops = true;
  const float uniformStep = stops[startIndex].position - prev;

  for (size_t i = startIndex; i < stops.size(); ++i) {
    const bool isLast = (i + 1 == stops.size());
    const float clamped = isLast ? 1.0f : std::clamp(stops[i].position, prev, 1.0f);
    hasUniformStops &= NearEquals(clamped - prev, uniformStep);
    stops[i].position = clamped;
    prev = clamped;
  }

  return Expected<GradientData, std::string>::Success(
      GradientData{std::move(stops), colorsAreOpaque, hasUniformStops});
}

ShaderContext::ShaderContext(Shader shader, Transform inverseTransform)
    : shader_(std::move(shader)), inverseTransform_(inverseTransform) {
  if (shader_.kind() == ShaderKind::kLinearGradient) {
    linearDelta_ = shader_.linearGradient().end - shader_.linearGradient().start;
    linearLengthSquared_ = linearDelta_.lengthSquared();
  } else if (shader_.kind() == ShaderKind::kRadialGradient) {
    radialDelta_ = shader_.radialGradient().end - shader_.radialGradient().start;
    radialRadius_ = shader_.radialGradient().radius;
    radialA_ = radialDelta_.lengthSquared() - radialRadius_ * radialRadius_;
  } else if (shader_.kind() == ShaderKind::kPattern) {
    patternPixmap_ = shader_.pattern().pixmap;
    filterQuality_ = shader_.pattern().filterQuality;

    const bool isTranslateOnly = NearEquals(shader_.pattern().transform.data[0], 1.0) &&
                                 NearEquals(shader_.pattern().transform.data[3], 1.0) &&
                                 NearZero(shader_.pattern().transform.data[1]) &&
                                 NearZero(shader_.pattern().transform.data[2]);
    if (isTranslateOnly) {
      filterQuality_ = FilterQuality::kNearest;
    }
  }
}

Expected<ShaderContext, std::string> ShaderContext::Create(const Shader& shader) {
  if (shader.kind() == ShaderKind::kPattern && shader.pattern().pixmap == nullptr) {
    return Expected<ShaderContext, std::string>::Failure("pattern shader missing pixmap");
  }

  Transform inverseTransform;
  switch (shader.kind()) {
    case ShaderKind::kSolidColor: inverseTransform = Transform(); break;
    case ShaderKind::kLinearGradient: {
      const double det = shader.linearGradient().transform.determinant();
      if (NearZero(det, std::numeric_limits<double>::epsilon())) {
        return Expected<ShaderContext, std::string>::Failure("shader transform is not invertible");
      }
      inverseTransform = shader.linearGradient().transform.inverse();
      break;
    }
    case ShaderKind::kRadialGradient: {
      const double det = shader.radialGradient().transform.determinant();
      if (NearZero(det, std::numeric_limits<double>::epsilon())) {
        return Expected<ShaderContext, std::string>::Failure("shader transform is not invertible");
      }
      inverseTransform = shader.radialGradient().transform.inverse();
      break;
    }
    case ShaderKind::kPattern: {
      const double det = shader.pattern().transform.determinant();
      if (NearZero(det, std::numeric_limits<double>::epsilon())) {
        return Expected<ShaderContext, std::string>::Failure("shader transform is not invertible");
      }
      inverseTransform = shader.pattern().transform.inverse();
      break;
    }
  }

  return Expected<ShaderContext, std::string>::Success(ShaderContext(shader, inverseTransform));
}

Color ShaderContext::sample(const Vector2d& position) const {
  switch (shader_.kind()) {
    case ShaderKind::kSolidColor: return shader_.solidColor().color;
    case ShaderKind::kLinearGradient: return sampleLinear(position);
    case ShaderKind::kRadialGradient: return sampleRadial(position);
    case ShaderKind::kPattern: return samplePattern(position);
  }
  return {};
}

bool ShaderContext::sampleLinearSpan(int x, int y, int width, std::vector<Color>& outColors) const {
  if (shader_.kind() != ShaderKind::kLinearGradient || width <= 0) {
    return false;
  }

  if (shader_.linearGradient().spreadMode != SpreadMode::kPad) {
    return false;
  }

  outColors.resize(static_cast<size_t>(width));

  const Vector2d deviceStart(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
  const Vector2d localStart = inverseTransform_.transformPosition(deviceStart);
  const Vector2d step = inverseTransform_.transformVector(Vector2d(1.0, 0.0));

  sampleLinearFastPath(localStart, step, width, outColors);
  return true;
}

Color ShaderContext::sampleLinear(const Vector2d& position) const {
  const Vector2d local = inverseTransform_.transformPosition(position);
  const Vector2d start = shader_.linearGradient().start;
  const Vector2d delta = linearDelta_;

  if (NearZero(linearLengthSquared_)) {
    return shader_.linearGradient().gradient.stops.back().color;
  }

  const double tNumerator = (local - start).dot(delta);
  float t = static_cast<float>(tNumerator / linearLengthSquared_);
  t = applySpread(t, shader_.linearGradient().spreadMode);

  return sampleGradient(shader_.linearGradient().gradient, t);
}

void ShaderContext::sampleLinearFastPath(const Vector2d& start, const Vector2d& step, int width,
                                         std::vector<Color>& outColors) const {
  [[maybe_unused]] const CpuFeatures& cpuFeatures = GetCpuFeatures();
#if defined(DONNER_ENABLE_TINY_SKIA_AVX2) && defined(__AVX2__) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
  if (cpuFeatures.hasAvx2 && sampleLinearFastPathAvx2(start, step, width, outColors)) {
    return;
  }
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
  if (cpuFeatures.hasSse2 && sampleLinearFastPathSimd(start, step, width, outColors)) {
    return;
  }
#endif
#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && defined(__ARM_NEON) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
  if (cpuFeatures.hasNeon && sampleLinearFastPathNeon(start, step, width, outColors)) {
    return;
  }
#endif

  if (NearZero(linearLengthSquared_)) {
    std::fill(outColors.begin(), outColors.end(),
              shader_.linearGradient().gradient.stops.back().color);
    return;
  }

  const Vector2d delta = linearDelta_;
  const Vector2d origin = shader_.linearGradient().start;
  const double invLengthSquared = 1.0 / linearLengthSquared_;

  Vector2d local = start;
  float t = static_cast<float>((local - origin).dot(delta) * invLengthSquared);
  const float tStep = static_cast<float>(step.dot(delta) * invLengthSquared);

  const GradientData& gradient = shader_.linearGradient().gradient;
  size_t stopIndex = FindInitialStopIndex(gradient, applySpread(t, SpreadMode::kPad));

  for (int i = 0; i < width; ++i) {
    const float spreadT = applySpread(t, SpreadMode::kPad);
    outColors[static_cast<size_t>(i)] =
        SampleGradientWithIndex(gradient, spreadT, tStep, stopIndex);
    t += tStep;
  }
}

#if defined(DONNER_ENABLE_TINY_SKIA_AVX2) && defined(__AVX2__) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
bool ShaderContext::sampleLinearFastPathAvx2(const Vector2d& start, const Vector2d& step, int width,
                                             std::vector<Color>& outColors) const {
  const GradientData& gradient = shader_.linearGradient().gradient;
  if (gradient.stops.size() != 2) {
    return false;
  }

  const GradientStop& left = gradient.stops.front();
  const GradientStop& right = gradient.stops.back();
  if (!NearEquals(left.position, 0.0f) || !NearEquals(right.position, 1.0f)) {
    return false;
  }

  if (NearZero(linearLengthSquared_)) {
    std::fill(outColors.begin(), outColors.end(), right.color);
    return true;
  }

  const Vector2d delta = linearDelta_;
  const Vector2d origin = shader_.linearGradient().start;
  const double invLengthSquared = 1.0 / linearLengthSquared_;

  Vector2d local = start;
  float t = static_cast<float>((local - origin).dot(delta) * invLengthSquared);
  const float tStep = static_cast<float>(step.dot(delta) * invLengthSquared);

  const __m256 stepOffsets = _mm256_set_ps(7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f);
  const __m256 tStepVec = _mm256_set1_ps(tStep);
  const __m256 zero = _mm256_set1_ps(0.0f);
  const __m256 one = _mm256_set1_ps(1.0f);

  const __m256 startR = _mm256_set1_ps(static_cast<float>(left.color.r));
  const __m256 startG = _mm256_set1_ps(static_cast<float>(left.color.g));
  const __m256 startB = _mm256_set1_ps(static_cast<float>(left.color.b));
  const __m256 startA = _mm256_set1_ps(static_cast<float>(left.color.a));

  const __m256 deltaR = _mm256_set1_ps(static_cast<float>(right.color.r - left.color.r));
  const __m256 deltaG = _mm256_set1_ps(static_cast<float>(right.color.g - left.color.g));
  const __m256 deltaB = _mm256_set1_ps(static_cast<float>(right.color.b - left.color.b));
  const __m256 deltaA = _mm256_set1_ps(static_cast<float>(right.color.a - left.color.a));

  int pixelIndex = 0;
  alignas(32) float r[8];
  alignas(32) float g[8];
  alignas(32) float b[8];
  alignas(32) float a[8];

  while (pixelIndex + 7 < width) {
    const __m256 tBase = _mm256_set1_ps(t);
    __m256 tValues = _mm256_add_ps(tBase, _mm256_mul_ps(stepOffsets, tStepVec));
    tValues = _mm256_max_ps(zero, _mm256_min_ps(one, tValues));

    const __m256 rVec = _mm256_add_ps(startR, _mm256_mul_ps(deltaR, tValues));
    const __m256 gVec = _mm256_add_ps(startG, _mm256_mul_ps(deltaG, tValues));
    const __m256 bVec = _mm256_add_ps(startB, _mm256_mul_ps(deltaB, tValues));
    const __m256 aVec = _mm256_add_ps(startA, _mm256_mul_ps(deltaA, tValues));

    _mm256_store_ps(r, rVec);
    _mm256_store_ps(g, gVec);
    _mm256_store_ps(b, bVec);
    _mm256_store_ps(a, aVec);

    for (int lane = 0; lane < 8; ++lane) {
      const int outIndex = pixelIndex + lane;
      outColors[static_cast<size_t>(outIndex)] = Color(ClampToByte(r[lane]), ClampToByte(g[lane]),
                                                       ClampToByte(b[lane]), ClampToByte(a[lane]));
    }

    t += 8.0f * tStep;
    pixelIndex += 8;
  }

  for (; pixelIndex < width; ++pixelIndex) {
    const float clampedT = ClampToUnit(t);
    outColors[static_cast<size_t>(pixelIndex)] = LerpColors(left.color, right.color, clampedT);
    t += tStep;
  }

  return true;
}
#endif

#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
bool ShaderContext::sampleLinearFastPathSimd(const Vector2d& start, const Vector2d& step, int width,
                                             std::vector<Color>& outColors) const {
  const GradientData& gradient = shader_.linearGradient().gradient;
  if (gradient.stops.size() != 2) {
    return false;
  }

  const GradientStop& left = gradient.stops.front();
  const GradientStop& right = gradient.stops.back();
  if (!NearEquals(left.position, 0.0f) || !NearEquals(right.position, 1.0f)) {
    return false;
  }

  if (NearZero(linearLengthSquared_)) {
    std::fill(outColors.begin(), outColors.end(), right.color);
    return true;
  }

  const Vector2d delta = linearDelta_;
  const Vector2d origin = shader_.linearGradient().start;
  const double invLengthSquared = 1.0 / linearLengthSquared_;

  Vector2d local = start;
  float t = static_cast<float>((local - origin).dot(delta) * invLengthSquared);
  const float tStep = static_cast<float>(step.dot(delta) * invLengthSquared);

  const __m128 stepOffsets = _mm_set_ps(3.0f, 2.0f, 1.0f, 0.0f);
  const __m128 tStepVec = _mm_set1_ps(tStep);
  const __m128 zero = _mm_set1_ps(0.0f);
  const __m128 one = _mm_set1_ps(1.0f);

  const __m128 startR = _mm_set1_ps(static_cast<float>(left.color.r));
  const __m128 startG = _mm_set1_ps(static_cast<float>(left.color.g));
  const __m128 startB = _mm_set1_ps(static_cast<float>(left.color.b));
  const __m128 startA = _mm_set1_ps(static_cast<float>(left.color.a));

  const __m128 deltaR = _mm_set1_ps(static_cast<float>(right.color.r - left.color.r));
  const __m128 deltaG = _mm_set1_ps(static_cast<float>(right.color.g - left.color.g));
  const __m128 deltaB = _mm_set1_ps(static_cast<float>(right.color.b - left.color.b));
  const __m128 deltaA = _mm_set1_ps(static_cast<float>(right.color.a - left.color.a));

  int pixelIndex = 0;
  alignas(16) float r[4];
  alignas(16) float g[4];
  alignas(16) float b[4];
  alignas(16) float a[4];

  while (pixelIndex + 3 < width) {
    const __m128 tBase = _mm_set1_ps(t);
    __m128 tValues = _mm_add_ps(tBase, _mm_mul_ps(stepOffsets, tStepVec));
    tValues = _mm_max_ps(zero, _mm_min_ps(one, tValues));

    const __m128 rVec = _mm_add_ps(startR, _mm_mul_ps(deltaR, tValues));
    const __m128 gVec = _mm_add_ps(startG, _mm_mul_ps(deltaG, tValues));
    const __m128 bVec = _mm_add_ps(startB, _mm_mul_ps(deltaB, tValues));
    const __m128 aVec = _mm_add_ps(startA, _mm_mul_ps(deltaA, tValues));

    _mm_store_ps(r, rVec);
    _mm_store_ps(g, gVec);
    _mm_store_ps(b, bVec);
    _mm_store_ps(a, aVec);

    for (int lane = 0; lane < 4; ++lane) {
      const int outIndex = pixelIndex + lane;
      outColors[static_cast<size_t>(outIndex)] = Color(ClampToByte(r[lane]), ClampToByte(g[lane]),
                                                       ClampToByte(b[lane]), ClampToByte(a[lane]));
    }

    t += 4.0f * tStep;
    pixelIndex += 4;
  }

  for (; pixelIndex < width; ++pixelIndex) {
    const float clampedT = ClampToUnit(t);
    outColors[static_cast<size_t>(pixelIndex)] = LerpColors(left.color, right.color, clampedT);
    t += tStep;
  }

  return true;
}
#endif

#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && defined(__ARM_NEON) && DONNER_TINY_SKIA_ENABLE_SHADER_SIMD
bool ShaderContext::sampleLinearFastPathNeon(const Vector2d& start, const Vector2d& step, int width,
                                             std::vector<Color>& outColors) const {
  const GradientData& gradient = shader_.linearGradient().gradient;
  if (gradient.stops.size() != 2) {
    return false;
  }

  const GradientStop& left = gradient.stops.front();
  const GradientStop& right = gradient.stops.back();
  if (!NearEquals(left.position, 0.0f) || !NearEquals(right.position, 1.0f)) {
    return false;
  }

  if (NearZero(linearLengthSquared_)) {
    std::fill(outColors.begin(), outColors.end(), right.color);
    return true;
  }

  const Vector2d delta = linearDelta_;
  const Vector2d origin = shader_.linearGradient().start;
  const double invLengthSquared = 1.0 / linearLengthSquared_;

  Vector2d local = start;
  float t = static_cast<float>((local - origin).dot(delta) * invLengthSquared);
  const float tStep = static_cast<float>(step.dot(delta) * invLengthSquared);

  const float32x4_t stepOffsets = {0.0f, 1.0f, 2.0f, 3.0f};
  const float32x4_t tStepVec = vdupq_n_f32(tStep);
  const float32x4_t zero = vdupq_n_f32(0.0f);
  const float32x4_t one = vdupq_n_f32(1.0f);

  const float32x4_t startR = vdupq_n_f32(static_cast<float>(left.color.r));
  const float32x4_t startG = vdupq_n_f32(static_cast<float>(left.color.g));
  const float32x4_t startB = vdupq_n_f32(static_cast<float>(left.color.b));
  const float32x4_t startA = vdupq_n_f32(static_cast<float>(left.color.a));

  const float32x4_t deltaR = vdupq_n_f32(static_cast<float>(right.color.r - left.color.r));
  const float32x4_t deltaG = vdupq_n_f32(static_cast<float>(right.color.g - left.color.g));
  const float32x4_t deltaB = vdupq_n_f32(static_cast<float>(right.color.b - left.color.b));
  const float32x4_t deltaA = vdupq_n_f32(static_cast<float>(right.color.a - left.color.a));

  int pixelIndex = 0;
  alignas(16) float r[4];
  alignas(16) float g[4];
  alignas(16) float b[4];
  alignas(16) float a[4];

  while (pixelIndex + 3 < width) {
    const float32x4_t tBase = vdupq_n_f32(t);
    float32x4_t tValues = vmlaq_f32(tBase, stepOffsets, tStepVec);
    tValues = vmaxq_f32(zero, vminq_f32(one, tValues));

    const float32x4_t rVec = vmlaq_f32(startR, deltaR, tValues);
    const float32x4_t gVec = vmlaq_f32(startG, deltaG, tValues);
    const float32x4_t bVec = vmlaq_f32(startB, deltaB, tValues);
    const float32x4_t aVec = vmlaq_f32(startA, deltaA, tValues);

    vst1q_f32(r, rVec);
    vst1q_f32(g, gVec);
    vst1q_f32(b, bVec);
    vst1q_f32(a, aVec);

    for (int lane = 0; lane < 4; ++lane) {
      const int outIndex = pixelIndex + lane;
      outColors[static_cast<size_t>(outIndex)] = Color(ClampToByte(r[lane]), ClampToByte(g[lane]),
                                                       ClampToByte(b[lane]), ClampToByte(a[lane]));
    }

    t += 4.0f * tStep;
    pixelIndex += 4;
  }

  for (; pixelIndex < width; ++pixelIndex) {
    const float clampedT = ClampToUnit(t);
    outColors[static_cast<size_t>(pixelIndex)] = LerpColors(left.color, right.color, clampedT);
    t += tStep;
  }

  return true;
}
#endif

Color ShaderContext::sampleRadial(const Vector2d& position) const {
  const Vector2d local = inverseTransform_.transformPosition(position);
  const Vector2d start = shader_.radialGradient().start;
  const Vector2d toPoint = local - start;

  if (NearZero(radialRadius_)) {
    return shader_.radialGradient().gradient.stops.back().color;
  }

  const double b = -2.0 * toPoint.dot(radialDelta_);
  const double c = toPoint.lengthSquared();

  std::optional<double> tValue;
  if (NearZero(radialA_)) {
    if (!NearZero(b)) {
      tValue = -c / b;
    }
  } else {
    const double discriminant = b * b - 4.0 * radialA_ * c;
    if (discriminant >= 0.0) {
      const double sqrtDisc = std::sqrt(discriminant);
      const double t0 = (-b - sqrtDisc) / (2.0 * radialA_);
      const double t1 = (-b + sqrtDisc) / (2.0 * radialA_);

      const double positive = t0 >= 0.0 && std::isfinite(t0) ? t0 : t1;
      if (positive >= 0.0 && std::isfinite(positive)) {
        tValue = positive;
      }
    }
  }

  if (!tValue.has_value()) {
    return shader_.radialGradient().gradient.stops.back().color;
  }

  float t = static_cast<float>(*tValue);
  t = applySpread(t, shader_.radialGradient().spreadMode);
  return sampleGradient(shader_.radialGradient().gradient, t);
}

Color ShaderContext::samplePattern(const Vector2d& position) const {
  if (patternPixmap_ == nullptr || !patternPixmap_->isValid()) {
    return {};
  }

  const Vector2d local = inverseTransform_.transformPosition(position);

  Color sampled;
  switch (filterQuality_) {
    case FilterQuality::kNearest: sampled = sampleNearest(local); break;
    case FilterQuality::kBilinear: sampled = sampleBilinear(local); break;
    case FilterQuality::kBicubic: sampled = sampleBicubic(local); break;
  }

  return MultiplyColor(sampled, shader_.pattern().opacity);
}

Color ShaderContext::sampleNearest(const Vector2d& local) const {
  const double x = std::floor(local.x + 0.5);
  const double y = std::floor(local.y + 0.5);
  return sampleWithSpread(x, y);
}

Color ShaderContext::sampleBilinear(const Vector2d& local) const {
  const double fx = std::floor(local.x);
  const double fy = std::floor(local.y);
  const double dx = local.x - fx;
  const double dy = local.y - fy;

  const Color c00 = sampleWithSpread(fx, fy);
  const Color c10 = sampleWithSpread(fx + 1.0, fy);
  const Color c01 = sampleWithSpread(fx, fy + 1.0);
  const Color c11 = sampleWithSpread(fx + 1.0, fy + 1.0);

  const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };

  Color result;
  result.r = ClampToByte(lerp(lerp(c00.r, c10.r, dx), lerp(c01.r, c11.r, dx), dy));
  result.g = ClampToByte(lerp(lerp(c00.g, c10.g, dx), lerp(c01.g, c11.g, dx), dy));
  result.b = ClampToByte(lerp(lerp(c00.b, c10.b, dx), lerp(c01.b, c11.b, dx), dy));
  result.a = ClampToByte(lerp(lerp(c00.a, c10.a, dx), lerp(c01.a, c11.a, dx), dy));
  return result;
}

Color ShaderContext::sampleBicubic(const Vector2d& local) const {
  const double fx = std::floor(local.x);
  const double fy = std::floor(local.y);

  const auto cubicWeight = [](double t) {
    const double a = std::abs(t);
    if (a <= 1.0) {
      return (1.5 * a - 2.5) * a * a + 1.0;
    }
    if (a < 2.0) {
      return ((-0.5 * a + 2.5) * a - 4.0) * a + 2.0;
    }
    return 0.0;
  };

  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 0.0;
  double weightSum = 0.0;

  for (int y = -1; y <= 2; ++y) {
    const double wy = cubicWeight(local.y - (fy + static_cast<double>(y)));
    for (int x = -1; x <= 2; ++x) {
      const double wx = cubicWeight(local.x - (fx + static_cast<double>(x)));
      const double weight = wx * wy;
      if (NearZero(weight)) {
        continue;
      }

      const Color c = sampleWithSpread(fx + static_cast<double>(x), fy + static_cast<double>(y));
      r += weight * static_cast<double>(c.r);
      g += weight * static_cast<double>(c.g);
      b += weight * static_cast<double>(c.b);
      a += weight * static_cast<double>(c.a);
      weightSum += weight;
    }
  }

  if (NearZero(weightSum)) {
    return {};
  }

  const double invWeight = 1.0 / weightSum;
  return Color{ClampToByte(r * invWeight), ClampToByte(g * invWeight), ClampToByte(b * invWeight),
               ClampToByte(a * invWeight)};
}

Color ShaderContext::sampleWithSpread(double x, double y) const {
  if (patternPixmap_ == nullptr || !patternPixmap_->isValid()) {
    return {};
  }

  const double limitX = static_cast<double>(patternPixmap_->width() - 1);
  const double limitY = static_cast<double>(patternPixmap_->height() - 1);

  const double adjustedX =
      applySpreadToCoordinate(x, patternPixmap_->width(), shader_.pattern().spreadMode);
  const double adjustedY =
      applySpreadToCoordinate(y, patternPixmap_->height(), shader_.pattern().spreadMode);

  const int ix = static_cast<int>(std::clamp(std::floor(adjustedX), 0.0, limitX));
  const int iy = static_cast<int>(std::clamp(std::floor(adjustedY), 0.0, limitY));

  const std::span<const uint8_t> pixels = patternPixmap_->pixels();
  const size_t offset =
      patternPixmap_->strideBytes() * static_cast<size_t>(iy) + static_cast<size_t>(ix) * 4;
  return Color{pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

double ShaderContext::applySpreadToCoordinate(double coordinate, double extent,
                                              SpreadMode spreadMode) {
  const double limit = std::max(0.0, extent - 1.0);
  switch (spreadMode) {
    case SpreadMode::kPad: return std::clamp(coordinate, 0.0, limit);
    case SpreadMode::kRepeat: {
      if (extent <= 0.0) {
        return 0.0;
      }
      double wrapped = std::fmod(coordinate, extent);
      if (wrapped < 0.0) {
        wrapped += extent;
      }
      return std::clamp(wrapped, 0.0, limit);
    }
    case SpreadMode::kReflect: {
      if (extent <= 0.0) {
        return 0.0;
      }
      const double period = extent * 2.0;
      double wrapped = std::fmod(coordinate, period);
      if (wrapped < 0.0) {
        wrapped += period;
      }
      const double mirrored = wrapped <= extent ? wrapped : period - wrapped;
      return std::clamp(mirrored, 0.0, limit);
    }
  }

  return std::clamp(coordinate, 0.0, limit);
}

Color ShaderContext::sampleGradient(const GradientData& gradient, float t) {
  const float clamped = ClampToUnit(t);

  for (size_t i = 1; i < gradient.stops.size(); ++i) {
    const GradientStop& left = gradient.stops[i - 1];
    const GradientStop& right = gradient.stops[i];

    if (clamped < right.position || NearEquals(clamped, right.position)) {
      const float span = right.position - left.position;
      if (NearZero(span)) {
        return right.color;
      }

      const float ratio = (clamped - left.position) / span;
      return LerpColors(left.color, right.color, ratio);
    }
  }

  return gradient.stops.back().color;
}

float ShaderContext::applySpread(float t, SpreadMode spreadMode) {
  switch (spreadMode) {
    case SpreadMode::kPad: return ClampToUnit(t);
    case SpreadMode::kRepeat: {
      const float wrapped = t - std::floor(t);
      return ClampToUnit(wrapped);
    }
    case SpreadMode::kReflect: {
      const float mirrored = std::fabs(std::fmod(t, 2.0f));
      const float interval = mirrored > 1.0f ? 2.0f - mirrored : mirrored;
      return ClampToUnit(interval);
    }
  }

  return ClampToUnit(t);
}

}  // namespace donner::backends::tiny_skia_cpp
