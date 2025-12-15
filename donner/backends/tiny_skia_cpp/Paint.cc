#include "donner/backends/tiny_skia_cpp/Paint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#if defined(DONNER_ENABLE_TINY_SKIA_AVX2) && defined(__AVX2__)
#include <immintrin.h>
#define DONNER_TINY_SKIA_USE_AVX2 1
#endif

#if defined(DONNER_ENABLE_TINY_SKIA_SSE2) && defined(__SSE2__)
#include <emmintrin.h>
#define DONNER_TINY_SKIA_USE_SSE2 1
#endif

#if defined(DONNER_ENABLE_TINY_SKIA_NEON) && defined(__ARM_NEON)
#include <arm_neon.h>
#define DONNER_TINY_SKIA_USE_NEON 1
#endif

#define DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD 0

#include "donner/backends/tiny_skia_cpp/CpuFeatures.h"
#include "donner/backends/tiny_skia_cpp/Wide.h"

namespace donner::backends::tiny_skia_cpp {

namespace {

std::array<Color, 4> LoadColors(const uint8_t* bytes);
Color BlendSourceOverSimd(const Color& src, const Color& dest);

Color ClampFromVector(const F32x4& vector) {
  const std::array<float, 4> channels = vector.toArray();
  return Color{static_cast<uint8_t>(std::lround(std::clamp(channels[0], 0.0f, 255.0f))),
               static_cast<uint8_t>(std::lround(std::clamp(channels[1], 0.0f, 255.0f))),
               static_cast<uint8_t>(std::lround(std::clamp(channels[2], 0.0f, 255.0f))),
               static_cast<uint8_t>(std::lround(std::clamp(channels[3], 0.0f, 255.0f)))};
}

Color MultiplyColor(Color color, float scale) {
  const float clamped = std::clamp(scale, 0.0f, 1.0f);
  const F32x4 scaled = F32x4::FromColor(color) * clamped;
  return ClampFromVector(scaled);
}

struct PremultipliedBlock {
  F32x4 r;
  F32x4 g;
  F32x4 b;
  F32x4 alpha01;
};

PremultipliedBlock PremultiplyBlock(const Color* colors, const uint8_t* coverage) {
  std::array<float, 4> r{};
  std::array<float, 4> g{};
  std::array<float, 4> b{};
  std::array<float, 4> alpha01{};

  for (size_t i = 0; i < 4; ++i) {
    Color color = colors[i];
    if (coverage != nullptr) {
      const float maskScale = static_cast<float>(coverage[i]) / 255.0f;
      color = MultiplyColor(color, maskScale);
    }

    const int alpha = color.a;
    const int premultR = (color.r * alpha + 127) / 255;
    const int premultG = (color.g * alpha + 127) / 255;
    const int premultB = (color.b * alpha + 127) / 255;

    r[i] = static_cast<float>(premultR);
    g[i] = static_cast<float>(premultG);
    b[i] = static_cast<float>(premultB);
    alpha01[i] = static_cast<float>(alpha) / 255.0f;
  }

  return {F32x4::FromArray(r), F32x4::FromArray(g), F32x4::FromArray(b), F32x4::FromArray(alpha01)};
}

void BlendSourceOverBlock(uint8_t* dst, const PremultipliedBlock& src,
                          const PremultipliedBlock& dest) {
  const F32x4 invAlpha = F32x4::Splat(1.0f) - src.alpha01;
  const F32x4 outR = src.r + dest.r * invAlpha;
  const F32x4 outG = src.g + dest.g * invAlpha;
  const F32x4 outB = src.b + dest.b * invAlpha;
  const F32x4 outA = src.alpha01 + dest.alpha01 * invAlpha;

  const std::array<float, 4> r = outR.toArray();
  const std::array<float, 4> g = outG.toArray();
  const std::array<float, 4> b = outB.toArray();
  const std::array<float, 4> a = outA.toArray();

  for (size_t i = 0; i < 4; ++i) {
    const uint8_t clampedR = static_cast<uint8_t>(std::lround(std::clamp(r[i], 0.0f, 255.0f)));
    const uint8_t clampedG = static_cast<uint8_t>(std::lround(std::clamp(g[i], 0.0f, 255.0f)));
    const uint8_t clampedB = static_cast<uint8_t>(std::lround(std::clamp(b[i], 0.0f, 255.0f)));
    const uint8_t clampedA =
        static_cast<uint8_t>(std::lround(std::clamp(a[i] * 255.0f, 0.0f, 255.0f)));

    dst[i * 4] = clampedR;
    dst[i * 4 + 1] = clampedG;
    dst[i * 4 + 2] = clampedB;
    dst[i * 4 + 3] = clampedA;
  }
}

#if defined(DONNER_TINY_SKIA_USE_SSE2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
bool BlendSourceOverBlockSse2(uint8_t* dst, const Color* srcColors, const uint8_t* coverage) {
  const bool hasCoverage = coverage != nullptr;
  if (hasCoverage && coverage[0] == 0 && coverage[1] == 0 && coverage[2] == 0 && coverage[3] == 0) {
    return true;
  }

  const std::array<Color, 4> destColors = LoadColors(dst);
  const PremultipliedBlock srcBlock = PremultiplyBlock(srcColors, coverage);
  const PremultipliedBlock destBlock = PremultiplyBlock(destColors.data(), nullptr);
  BlendSourceOverBlock(dst, srcBlock, destBlock);

  if (hasCoverage) {
    for (int lane = 0; lane < 4; ++lane) {
      if (coverage[lane] != 0) {
        continue;
      }

      const size_t offset = static_cast<size_t>(lane) * 4;
      dst[offset] = destColors[static_cast<size_t>(lane)].r;
      dst[offset + 1] = destColors[static_cast<size_t>(lane)].g;
      dst[offset + 2] = destColors[static_cast<size_t>(lane)].b;
      dst[offset + 3] = destColors[static_cast<size_t>(lane)].a;
    }
  }

  return true;
}
#endif

#if defined(DONNER_TINY_SKIA_USE_NEON) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
bool BlendSourceOverBlockNeon(uint8_t* dst, const Color* srcColors, const uint8_t* coverage) {
  alignas(16) float srcR[4];
  alignas(16) float srcG[4];
  alignas(16) float srcB[4];
  alignas(16) float srcA[4];
  alignas(16) float destR[4];
  alignas(16) float destG[4];
  alignas(16) float destB[4];
  alignas(16) float destA[4];
  Color destColors[4];

  const bool hasCoverage = coverage != nullptr;
  bool zeroCoverage = hasCoverage;

  for (int lane = 0; lane < 4; ++lane) {
    const float maskScale = hasCoverage ? static_cast<float>(coverage[lane]) / 255.0f : 1.0f;
    zeroCoverage = zeroCoverage && maskScale <= 0.0f;

    const Color src = srcColors[lane];
    const float clampedScale = std::clamp(maskScale, 0.0f, 1.0f);
    const int scaledAlpha = static_cast<int>(std::lround(clampedScale * src.a));
    const int scaledR = static_cast<int>(std::lround(clampedScale * src.r));
    const int scaledG = static_cast<int>(std::lround(clampedScale * src.g));
    const int scaledB = static_cast<int>(std::lround(clampedScale * src.b));
    const int srcPremultR = (scaledR * scaledAlpha + 127) / 255;
    const int srcPremultG = (scaledG * scaledAlpha + 127) / 255;
    const int srcPremultB = (scaledB * scaledAlpha + 127) / 255;

    srcR[lane] = static_cast<float>(srcPremultR);
    srcG[lane] = static_cast<float>(srcPremultG);
    srcB[lane] = static_cast<float>(srcPremultB);
    srcA[lane] = static_cast<float>(scaledAlpha) / 255.0f;

    const size_t offset = static_cast<size_t>(lane) * 4;
    destColors[lane] = Color{dst[offset], dst[offset + 1], dst[offset + 2], dst[offset + 3]};
    const int destPremultR = (destColors[lane].r * destColors[lane].a + 127) / 255;
    const int destPremultG = (destColors[lane].g * destColors[lane].a + 127) / 255;
    const int destPremultB = (destColors[lane].b * destColors[lane].a + 127) / 255;

    destR[lane] = static_cast<float>(destPremultR);
    destG[lane] = static_cast<float>(destPremultG);
    destB[lane] = static_cast<float>(destPremultB);
    destA[lane] = static_cast<float>(destColors[lane].a) / 255.0f;
  }

  if (zeroCoverage) {
    return true;
  }

  const float32x4_t srcAlpha = vld1q_f32(srcA);
  const float32x4_t invAlpha = vsubq_f32(vdupq_n_f32(1.0f), srcAlpha);
  const float32x4_t destAlpha = vld1q_f32(destA);
  const float32x4_t outAlpha = vmlaq_f32(srcAlpha, destAlpha, invAlpha);

  const float32x4_t outR = vmlaq_f32(vld1q_f32(srcR), vld1q_f32(destR), invAlpha);
  const float32x4_t outG = vmlaq_f32(vld1q_f32(srcG), vld1q_f32(destG), invAlpha);
  const float32x4_t outB = vmlaq_f32(vld1q_f32(srcB), vld1q_f32(destB), invAlpha);

  alignas(16) float r[4];
  alignas(16) float g[4];
  alignas(16) float b[4];
  alignas(16) float a[4];
  vst1q_f32(r, outR);
  vst1q_f32(g, outG);
  vst1q_f32(b, outB);
  vst1q_f32(a, outAlpha);

  for (int lane = 0; lane < 4; ++lane) {
    if (hasCoverage && coverage[lane] == 0) {
      const size_t offset = static_cast<size_t>(lane) * 4;
      dst[offset] = destColors[lane].r;
      dst[offset + 1] = destColors[lane].g;
      dst[offset + 2] = destColors[lane].b;
      dst[offset + 3] = destColors[lane].a;
      continue;
    }

    const size_t offset = static_cast<size_t>(lane) * 4;
    dst[offset] = static_cast<uint8_t>(std::lround(std::clamp(r[lane], 0.0f, 255.0f)));
    dst[offset + 1] = static_cast<uint8_t>(std::lround(std::clamp(g[lane], 0.0f, 255.0f)));
    dst[offset + 2] = static_cast<uint8_t>(std::lround(std::clamp(b[lane], 0.0f, 255.0f)));
    dst[offset + 3] = static_cast<uint8_t>(std::lround(std::clamp(a[lane] * 255.0f, 0.0f, 255.0f)));
  }

  return true;
}
#endif

#if defined(DONNER_TINY_SKIA_USE_AVX2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
bool BlendSourceOverBlockAvx2(uint8_t* dst, const Color* srcColors, const uint8_t* coverage) {
  alignas(32) float srcR[8];
  alignas(32) float srcG[8];
  alignas(32) float srcB[8];
  alignas(32) float srcA[8];
  alignas(32) float destR[8];
  alignas(32) float destG[8];
  alignas(32) float destB[8];
  alignas(32) float destA[8];
  Color destColors[8];

  const bool hasCoverage = coverage != nullptr;
  bool zeroCoverage = hasCoverage;

  for (int lane = 0; lane < 8; ++lane) {
    const float maskScale = hasCoverage ? static_cast<float>(coverage[lane]) / 255.0f : 1.0f;
    zeroCoverage = zeroCoverage && maskScale <= 0.0f;

    const Color src = srcColors[lane];
    const float clampedScale = std::clamp(maskScale, 0.0f, 1.0f);
    const int scaledAlpha = static_cast<int>(std::lround(clampedScale * src.a));
    const int scaledR = static_cast<int>(std::lround(clampedScale * src.r));
    const int scaledG = static_cast<int>(std::lround(clampedScale * src.g));
    const int scaledB = static_cast<int>(std::lround(clampedScale * src.b));
    const int srcPremultR = (scaledR * scaledAlpha + 127) / 255;
    const int srcPremultG = (scaledG * scaledAlpha + 127) / 255;
    const int srcPremultB = (scaledB * scaledAlpha + 127) / 255;

    srcR[lane] = static_cast<float>(srcPremultR);
    srcG[lane] = static_cast<float>(srcPremultG);
    srcB[lane] = static_cast<float>(srcPremultB);
    srcA[lane] = static_cast<float>(scaledAlpha) / 255.0f;

    const size_t offset = static_cast<size_t>(lane) * 4;
    destColors[lane] = Color{dst[offset], dst[offset + 1], dst[offset + 2], dst[offset + 3]};
    const int destPremultR = (destColors[lane].r * destColors[lane].a + 127) / 255;
    const int destPremultG = (destColors[lane].g * destColors[lane].a + 127) / 255;
    const int destPremultB = (destColors[lane].b * destColors[lane].a + 127) / 255;

    destR[lane] = static_cast<float>(destPremultR);
    destG[lane] = static_cast<float>(destPremultG);
    destB[lane] = static_cast<float>(destPremultB);
    destA[lane] = static_cast<float>(destColors[lane].a) / 255.0f;
  }

  if (zeroCoverage) {
    return true;
  }

  const __m256 srcAlpha = _mm256_load_ps(srcA);
  const __m256 invAlpha = _mm256_sub_ps(_mm256_set1_ps(1.0f), srcAlpha);
  const __m256 destAlpha = _mm256_load_ps(destA);
  const __m256 outAlpha = _mm256_add_ps(srcAlpha, _mm256_mul_ps(destAlpha, invAlpha));

  const __m256 outR =
      _mm256_add_ps(_mm256_load_ps(srcR), _mm256_mul_ps(_mm256_load_ps(destR), invAlpha));
  const __m256 outG =
      _mm256_add_ps(_mm256_load_ps(srcG), _mm256_mul_ps(_mm256_load_ps(destG), invAlpha));
  const __m256 outB =
      _mm256_add_ps(_mm256_load_ps(srcB), _mm256_mul_ps(_mm256_load_ps(destB), invAlpha));

  alignas(32) float r[8];
  alignas(32) float g[8];
  alignas(32) float b[8];
  alignas(32) float a[8];
  _mm256_store_ps(r, outR);
  _mm256_store_ps(g, outG);
  _mm256_store_ps(b, outB);
  _mm256_store_ps(a, outAlpha);

  for (int lane = 0; lane < 8; ++lane) {
    if (hasCoverage && coverage[lane] == 0) {
      const size_t offset = static_cast<size_t>(lane) * 4;
      dst[offset] = destColors[lane].r;
      dst[offset + 1] = destColors[lane].g;
      dst[offset + 2] = destColors[lane].b;
      dst[offset + 3] = destColors[lane].a;
      continue;
    }

    const size_t offset = static_cast<size_t>(lane) * 4;
    dst[offset] = static_cast<uint8_t>(std::lround(std::clamp(r[lane], 0.0f, 255.0f)));
    dst[offset + 1] = static_cast<uint8_t>(std::lround(std::clamp(g[lane], 0.0f, 255.0f)));
    dst[offset + 2] = static_cast<uint8_t>(std::lround(std::clamp(b[lane], 0.0f, 255.0f)));
    dst[offset + 3] = static_cast<uint8_t>(std::lround(std::clamp(a[lane] * 255.0f, 0.0f, 255.0f)));
  }

  return true;
}
#endif

void ShadeSpan(const PaintContext& paintContext, int startX, int y, int spanWidth,
               std::vector<Color>& shadedSpan) {
  shadedSpan.clear();
  if (paintContext.shadeLinearSpan(startX, y, spanWidth, shadedSpan)) {
    return;
  }

  shadedSpan.resize(static_cast<size_t>(spanWidth));
  for (int i = 0; i < spanWidth; ++i) {
    shadedSpan[static_cast<size_t>(i)] = paintContext.shade(
        Vector2d(static_cast<double>(startX + i) + 0.5, static_cast<double>(y) + 0.5));
  }
}

Color ApplySourceOpacity(const PaintContext& paintContext) {
  return paintContext.applyOpacity(paintContext.color());
}

template <size_t kSize>
void FillColorBlock(const Color& color, std::array<Color, kSize>& block) {
  for (size_t i = 0; i < kSize; ++i) {
    block[i] = color;
  }
}

void BlendSolidSourceOverSpan(uint8_t* row, int spanWidth, const Color& srcColor) {
  int dstX = 0;
  [[maybe_unused]] const CpuFeatures& cpuFeatures = GetCpuFeatures();
#if defined(DONNER_TINY_SKIA_USE_AVX2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
  if (cpuFeatures.hasAvx2) {
    std::array<Color, 8> avxColors{};
    FillColorBlock(srcColor, avxColors);
    while (spanWidth - dstX >= 8) {
      if (BlendSourceOverBlockAvx2(row + static_cast<size_t>(dstX) * 4, avxColors.data(),
                                   nullptr)) {
        dstX += 8;
        continue;
      }
      break;
    }
  }
#endif
  while (spanWidth - dstX >= 4) {
#if defined(DONNER_TINY_SKIA_USE_NEON) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (cpuFeatures.hasNeon) {
      std::array<Color, 4> neonColors{};
      FillColorBlock(srcColor, neonColors);
      if (BlendSourceOverBlockNeon(row + static_cast<size_t>(dstX) * 4, neonColors.data(),
                                   nullptr)) {
        dstX += 4;
        continue;
      }
    }
#endif
#if defined(DONNER_TINY_SKIA_USE_SSE2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (cpuFeatures.hasSse2) {
      std::array<Color, 4> sse2Colors{};
      FillColorBlock(srcColor, sse2Colors);
      if (BlendSourceOverBlockSse2(row + static_cast<size_t>(dstX) * 4, sse2Colors.data(),
                                   nullptr)) {
        dstX += 4;
        continue;
      }
    }
#endif
    std::array<Color, 4> scalarColors{};
    FillColorBlock(srcColor, scalarColors);
    const PremultipliedBlock srcBlock = PremultiplyBlock(scalarColors.data(), nullptr);
    const std::array<Color, 4> destColors = LoadColors(row + static_cast<size_t>(dstX) * 4);
    const PremultipliedBlock destBlock = PremultiplyBlock(destColors.data(), nullptr);
    BlendSourceOverBlock(row + static_cast<size_t>(dstX) * 4, srcBlock, destBlock);
    dstX += 4;
  }

  for (; dstX < spanWidth; ++dstX) {
    const size_t offset = static_cast<size_t>(dstX) * 4;
    const Color destColor{row[offset], row[offset + 1], row[offset + 2], row[offset + 3]};
    const Color outColor = BlendSourceOverSimd(srcColor, destColor);
    row[offset] = outColor.r;
    row[offset + 1] = outColor.g;
    row[offset + 2] = outColor.b;
    row[offset + 3] = outColor.a;
  }
}

void BlendSolidSourceOverMaskSpan(uint8_t* row, const uint8_t* coverage, int spanWidth,
                                  const Color& srcColor) {
  int dstX = 0;
  [[maybe_unused]] const CpuFeatures& cpuFeatures = GetCpuFeatures();
#if defined(DONNER_TINY_SKIA_USE_AVX2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
  if (cpuFeatures.hasAvx2) {
    std::array<Color, 8> avxColors{};
    FillColorBlock(srcColor, avxColors);
    while (spanWidth - dstX >= 8) {
      if (BlendSourceOverBlockAvx2(row + static_cast<size_t>(dstX) * 4, avxColors.data(),
                                   coverage + dstX)) {
        dstX += 8;
        continue;
      }
      break;
    }
  }
#endif
  while (spanWidth - dstX >= 4) {
#if defined(DONNER_TINY_SKIA_USE_NEON) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (cpuFeatures.hasNeon) {
      std::array<Color, 4> neonColors{};
      FillColorBlock(srcColor, neonColors);
      if (BlendSourceOverBlockNeon(row + static_cast<size_t>(dstX) * 4, neonColors.data(),
                                   coverage + dstX)) {
        dstX += 4;
        continue;
      }
    }
#endif
#if defined(DONNER_TINY_SKIA_USE_SSE2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (cpuFeatures.hasSse2) {
      std::array<Color, 4> sse2Colors{};
      FillColorBlock(srcColor, sse2Colors);
      if (BlendSourceOverBlockSse2(row + static_cast<size_t>(dstX) * 4, sse2Colors.data(),
                                   coverage + dstX)) {
        dstX += 4;
        continue;
      }
    }
#endif
    std::array<Color, 4> scalarColors{};
    FillColorBlock(srcColor, scalarColors);
    const PremultipliedBlock srcBlock = PremultiplyBlock(scalarColors.data(), coverage + dstX);
    const std::array<Color, 4> destColors = LoadColors(row + static_cast<size_t>(dstX) * 4);
    const PremultipliedBlock destBlock = PremultiplyBlock(destColors.data(), nullptr);
    BlendSourceOverBlock(row + static_cast<size_t>(dstX) * 4, srcBlock, destBlock);
    dstX += 4;
  }

  for (; dstX < spanWidth; ++dstX) {
    const size_t offset = static_cast<size_t>(dstX) * 4;
    const Color destColor{row[offset], row[offset + 1], row[offset + 2], row[offset + 3]};
    const float maskScale = static_cast<float>(coverage[dstX]) / 255.0f;
    const Color scaled = MultiplyColor(srcColor, maskScale);
    const Color outColor = BlendSourceOverSimd(scaled, destColor);
    row[offset] = outColor.r;
    row[offset + 1] = outColor.g;
    row[offset + 2] = outColor.b;
    row[offset + 3] = outColor.a;
  }
}

std::array<Color, 4> LoadColors(const uint8_t* bytes) {
  std::array<Color, 4> colors{};
  for (size_t i = 0; i < 4; ++i) {
    const size_t offset = i * 4;
    colors[i] = Color{bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]};
  }
  return colors;
}

Color BlendSourceOverSimd(const Color& src, const Color& dest) {
  const PremultipliedColorF srcPremult = Premultiply(src);
  const PremultipliedColorF destPremult = Premultiply(dest);

  const float invAlpha = static_cast<float>(1.0 - srcPremult.a);
  const F32x4 srcVector =
      F32x4::FromArray({static_cast<float>(srcPremult.r), static_cast<float>(srcPremult.g),
                        static_cast<float>(srcPremult.b), static_cast<float>(srcPremult.a)});
  const F32x4 destVector =
      F32x4::FromArray({static_cast<float>(destPremult.r), static_cast<float>(destPremult.g),
                        static_cast<float>(destPremult.b), static_cast<float>(destPremult.a)});

  const F32x4 blendedVector = srcVector + destVector * invAlpha;
  const std::array<float, 4> blended = blendedVector.toArray();
  return ToColor({blended[0], blended[1], blended[2], blended[3]});
}

}  // namespace

PaintContext::PaintContext(Paint paint, std::optional<ShaderContext> shaderContext,
                           float clampedOpacity)
    : paint_(std::move(paint)),
      shaderContext_(std::move(shaderContext)),
      opacity_(clampedOpacity) {}

Expected<PaintContext, std::string> PaintContext::Create(const Paint& paint) {
  std::optional<ShaderContext> shaderContext;
  if (paint.shader.has_value()) {
    auto context = ShaderContext::Create(*paint.shader);
    if (!context.hasValue()) {
      return Expected<PaintContext, std::string>::Failure(context.error());
    }
    shaderContext = context.value();
  }

  const float clampedOpacity = std::clamp(paint.opacity, 0.0f, 1.0f);
  return Expected<PaintContext, std::string>::Success(
      PaintContext(paint, std::move(shaderContext), clampedOpacity));
}

Color PaintContext::applyOpacity(Color color) const {
  return MultiplyColor(color, opacity_);
}

Color PaintContext::shade(const Vector2d& position) const {
  Color result = paint_.color;
  if (shaderContext_.has_value()) {
    result = shaderContext_->sample(position);
  }
  return applyOpacity(result);
}

bool PaintContext::shadeLinearSpan(int x, int y, int width, std::vector<Color>& outColors) const {
  if (!shaderContext_.has_value()) {
    return false;
  }

  if (!shaderContext_->sampleLinearSpan(x, y, width, outColors)) {
    return false;
  }

  for (Color& color : outColors) {
    color = applyOpacity(color);
  }

  return true;
}

void BlendSpan(Pixmap& pixmap, int x, int y, int width, const PaintContext& paintContext) {
  if (!pixmap.isValid() || width <= 0 || y < 0 || y >= pixmap.height()) {
    return;
  }

  const int startX = std::max(0, x);
  const int endX = std::min(pixmap.width(), x + width);
  if (startX >= endX) {
    return;
  }

  [[maybe_unused]] const CpuFeatures& cpuFeatures = GetCpuFeatures();
  const int spanWidth = endX - startX;
  uint8_t* const row = pixmap.data() + pixmap.strideBytes() * static_cast<size_t>(y);
  if (!paintContext.hasShader() && paintContext.blendMode() == BlendMode::kSourceOver) {
    const Color srcColor = ApplySourceOpacity(paintContext);
    BlendSolidSourceOverSpan(row + static_cast<size_t>(startX) * 4, spanWidth, srcColor);
    return;
  }
  std::vector<Color> shadedSpan;
  ShadeSpan(paintContext, startX, y, spanWidth, shadedSpan);

  int spanIndex = 0;
  for (int dstX = startX; dstX < endX; ++dstX) {
    const size_t offset = static_cast<size_t>(dstX) * 4;
    const Color destColor{row[offset], row[offset + 1], row[offset + 2], row[offset + 3]};
    const Color srcColor = shadedSpan[static_cast<size_t>(spanIndex++)];

    Color outColor;
#if defined(DONNER_TINY_SKIA_USE_AVX2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (paintContext.blendMode() == BlendMode::kSourceOver && cpuFeatures.hasAvx2 &&
        spanWidth - (dstX - startX) >= 8) {
      const Color* srcBlock = &shadedSpan[static_cast<size_t>(spanIndex - 1)];
      if (BlendSourceOverBlockAvx2(row + offset, srcBlock, nullptr)) {
        dstX += 7;
        spanIndex += 7;
        continue;
      }
    }
#endif
    if (paintContext.blendMode() == BlendMode::kSourceOver && spanWidth - (dstX - startX) >= 4) {
#if defined(DONNER_TINY_SKIA_USE_NEON) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
      if (cpuFeatures.hasNeon) {
        const Color* neonSrcBlock = &shadedSpan[static_cast<size_t>(spanIndex - 1)];
        if (BlendSourceOverBlockNeon(row + offset, neonSrcBlock, nullptr)) {
          dstX += 3;
          spanIndex += 3;
          continue;
        }
      }
#endif
#if defined(DONNER_TINY_SKIA_USE_SSE2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
      if (cpuFeatures.hasSse2) {
        const Color* sse2SrcBlock = &shadedSpan[static_cast<size_t>(spanIndex - 1)];
        if (BlendSourceOverBlockSse2(row + offset, sse2SrcBlock, nullptr)) {
          dstX += 3;
          spanIndex += 3;
          continue;
        }
      }
#endif
      const PremultipliedBlock srcBlock =
          PremultiplyBlock(&shadedSpan[static_cast<size_t>(spanIndex - 1)], nullptr);
      const std::array<Color, 4> destColors = LoadColors(row + offset);
      const PremultipliedBlock destBlock = PremultiplyBlock(destColors.data(), nullptr);
      BlendSourceOverBlock(row + offset, srcBlock, destBlock);
      dstX += 3;
      spanIndex += 3;
      continue;
    }

    if (paintContext.blendMode() == BlendMode::kSourceOver) {
      outColor = BlendSourceOverSimd(srcColor, destColor);
    } else {
      const PremultipliedColorF blended =
          Blend(Premultiply(srcColor), Premultiply(destColor), paintContext.blendMode());
      outColor = ToColor(blended);
    }
    row[offset] = outColor.r;
    row[offset + 1] = outColor.g;
    row[offset + 2] = outColor.b;
    row[offset + 3] = outColor.a;
  }
}

void BlendMaskSpan(Pixmap& pixmap, int x, int y, const uint8_t* coverage, int width,
                   const PaintContext& paintContext) {
  if (!pixmap.isValid() || coverage == nullptr || width <= 0 || y < 0 || y >= pixmap.height()) {
    return;
  }

  const int startX = std::max(0, x);
  const int endX = std::min(pixmap.width(), x + width);
  if (startX >= endX) {
    return;
  }

  const int spanWidth = endX - startX;
  const uint8_t* coverageCursor = coverage + std::max(0, -x);
  uint8_t* const row = pixmap.data() + pixmap.strideBytes() * static_cast<size_t>(y);
  [[maybe_unused]] const CpuFeatures& cpuFeatures = GetCpuFeatures();
  if (!paintContext.hasShader() && paintContext.blendMode() == BlendMode::kSourceOver) {
    const Color srcColor = ApplySourceOpacity(paintContext);
    BlendSolidSourceOverMaskSpan(row + static_cast<size_t>(startX) * 4, coverageCursor, spanWidth,
                                 srcColor);
    return;
  }
  std::vector<Color> shadedSpan;
  ShadeSpan(paintContext, startX, y, spanWidth, shadedSpan);

  int spanIndex = 0;
  for (int dstX = startX; dstX < endX; ++dstX) {
    const int remaining = endX - dstX;
    const size_t offset = static_cast<size_t>(dstX) * 4;

#if defined(DONNER_TINY_SKIA_USE_AVX2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
    if (paintContext.blendMode() == BlendMode::kSourceOver && cpuFeatures.hasAvx2 &&
        remaining >= 8) {
      if (BlendSourceOverBlockAvx2(row + offset, &shadedSpan[static_cast<size_t>(spanIndex)],
                                   coverageCursor)) {
        dstX += 7;
        spanIndex += 8;
        coverageCursor += 8;
        continue;
      }
    }
#endif
    if (paintContext.blendMode() == BlendMode::kSourceOver && remaining >= 4) {
#if defined(DONNER_TINY_SKIA_USE_NEON) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
      if (cpuFeatures.hasNeon) {
        if (BlendSourceOverBlockNeon(row + offset, &shadedSpan[static_cast<size_t>(spanIndex)],
                                     coverageCursor)) {
          dstX += 3;
          spanIndex += 4;
          coverageCursor += 4;
          continue;
        }
      }
#endif
#if defined(DONNER_TINY_SKIA_USE_SSE2) && DONNER_TINY_SKIA_ENABLE_BLOCK_SIMD
      if (cpuFeatures.hasSse2) {
        if (BlendSourceOverBlockSse2(row + offset, &shadedSpan[static_cast<size_t>(spanIndex)],
                                     coverageCursor)) {
          dstX += 3;
          spanIndex += 4;
          coverageCursor += 4;
          continue;
        }
      }
#endif
      const std::array<uint8_t, 4> blockCoverage = {coverageCursor[0], coverageCursor[1],
                                                    coverageCursor[2], coverageCursor[3]};
      const bool zeroCoverage = blockCoverage[0] == 0 && blockCoverage[1] == 0 &&
                                blockCoverage[2] == 0 && blockCoverage[3] == 0;
      if (!zeroCoverage) {
        const std::array<Color, 4> destColors = LoadColors(row + offset);
        const PremultipliedBlock srcBlock =
            PremultiplyBlock(&shadedSpan[static_cast<size_t>(spanIndex)], blockCoverage.data());
        const PremultipliedBlock destBlock = PremultiplyBlock(destColors.data(), nullptr);
        BlendSourceOverBlock(row + offset, srcBlock, destBlock);
        for (size_t lane = 0; lane < 4; ++lane) {
          if (blockCoverage[lane] == 0) {
            const size_t laneOffset = offset + lane * 4;
            row[laneOffset] = destColors[lane].r;
            row[laneOffset + 1] = destColors[lane].g;
            row[laneOffset + 2] = destColors[lane].b;
            row[laneOffset + 3] = destColors[lane].a;
          }
        }
      }

      dstX += 3;
      spanIndex += 4;
      coverageCursor += 4;
      continue;
    }

    const uint8_t mask = *coverageCursor++;
    if (mask == 0) {
      ++spanIndex;
      continue;
    }

    const Color destColor{row[offset], row[offset + 1], row[offset + 2], row[offset + 3]};

    const Color shaded = shadedSpan[static_cast<size_t>(spanIndex++)];
    const float maskScale = static_cast<float>(mask) / 255.0f;
    const Color scaled = MultiplyColor(shaded, maskScale);

    Color outColor;
    if (paintContext.blendMode() == BlendMode::kSourceOver) {
      outColor = BlendSourceOverSimd(scaled, destColor);
    } else {
      const PremultipliedColorF blended =
          Blend(Premultiply(scaled), Premultiply(destColor), paintContext.blendMode());
      outColor = ToColor(blended);
    }
    row[offset] = outColor.r;
    row[offset + 1] = outColor.g;
    row[offset + 2] = outColor.b;
    row[offset + 3] = outColor.a;
  }
}

}  // namespace donner::backends::tiny_skia_cpp
