/// Cross-validation of the filter primitives' vector branches against the
/// scalar arithmetic they replace.
///
/// Every expectation here is a longhand scalar reference written out in the
/// test, so the same assertions run against the vector branch in native mode
/// and against the fallback in scalar mode.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/Merge.h"
#include "tiny_skia/filter/SimdVec.h"
#include "tiny_skia/filter/Turbulence.h"
#include "tiny_skia/filter/TurbulenceKernel.h"

namespace tiny_skia::filter {
namespace {

using ::testing::ElementsAreArray;

// Whether the compiled branch evaluates the float expressions with exactly the
// scalar fallback's operations in the scalar fallback's order.
//
// The wasm128 and SSE2 branches are written to, and the scalar fallback
// trivially is. NEON is not: it fuses the multiply and add in the color-matrix
// and turbulence products, accumulates the color matrix's translation column
// first, and converts bytes to floats by multiplying by the rounded reciprocal
// of 255 rather than dividing (1/255 is not representable, and the two
// disagree in the last place for 126 of the 256 byte values). Those
// divergences are at most one unit in the last place, they predate this suite,
// and changing them would move rendered output on ARM, so the bitwise
// expectations are scoped to the branches that promise them.
#if defined(TINY_SKIA_SIMD_NEON)
constexpr bool kExactScalarParity = false;
#else
constexpr bool kExactScalarParity = true;
#endif

// Whether `FloatPixmap::toPixmap` runs its vector conversion. The scalar
// fallback casts the clamped float straight to uint8, and that cast is
// undefined behaviour for the NaN input the clamp passes through, so the
// expectations that feed a NaN through the conversion are scoped to the
// branches that define an answer for it.
#if defined(TINY_SKIA_SIMD_NEON) || defined(TINY_SKIA_SIMD_WASM_SIMD128) || \
    defined(TINY_SKIA_SIMD_SSE2)
constexpr bool kVectorToPixmap = true;
#else
constexpr bool kVectorToPixmap = false;
#endif

/// A quiet NaN, built from its bit pattern so no expression in the test has to
/// produce one.
float quietNan() { return std::bit_cast<float>(std::uint32_t{0x7fc00000}); }

/// Compares one float against its scalar reference.
///
/// Where the compiled branch promises bit-for-bit parity this compares the bit
/// patterns, so a single changed rounding fails.
///
/// Otherwise `tolerance` bounds the documented NEON divergence. It is an
/// absolute bound stated against the magnitudes entering the expression, not a
/// relative bound on the result: fusing a multiply and add moves the result by
/// at most one rounding of the largest intermediate, and these expressions
/// routinely cancel down to a result thousands of times smaller than their
/// terms, where a relative comparison would reject a correct one-unit
/// difference.
void expectFloatMatches(float actual, float expected, float tolerance) {
  if (kExactScalarParity) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(actual), std::bit_cast<std::uint32_t>(expected))
        << "actual=" << actual << " expected=" << expected;
  } else {
    EXPECT_NEAR(actual, expected, tolerance);
  }
}

/// Bound for expressions over inputs in [0, 1]: one rounding of 1.0 is about
/// 1.2e-7, so this leaves room for a handful of them.
constexpr float kUnitRangeTolerance = 1.0e-6f;

/// A deterministic byte sequence with no repeating structure that could hide a
/// lane-ordering mistake.
std::uint8_t sampleByte(std::size_t index) {
  return static_cast<std::uint8_t>((index * 37u + (index * index) / 3u) & 0xFFu);
}

// ---------------------------------------------------------------------------
// FloatPixmap conversions
// ---------------------------------------------------------------------------

Pixmap makePixmap(std::uint32_t width, std::uint32_t height, std::size_t seed) {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = sampleByte(i + seed);
  }

  return *Pixmap::fromVec(std::move(bytes), IntSize::fromWH(width, height).value());
}

TEST(FloatPixmapConversionTest, FromPixmapMatchesScalarDivide) {
  // Widths are chosen so the byte count leaves each possible remainder after
  // the 16-byte vector step: 4 pixels is exactly one step, and 5, 6, and 7
  // leave 4, 8, and 12 bytes for the tail.
  for (std::uint32_t width : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 16u}) {
    SCOPED_TRACE(testing::Message() << "width=" << width);

    const Pixmap source = makePixmap(width, 3, width * 13u);
    const FloatPixmap converted = FloatPixmap::fromPixmap(source);

    const auto src = source.data();
    const auto out = converted.data();
    ASSERT_EQ(out.size(), src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
      SCOPED_TRACE(testing::Message() << "i=" << i << " byte=" << int{src[i]});
      // The scalar conversion divides. Multiplying by the rounded reciprocal of
      // 255 instead lands one unit in the last place away for most byte values,
      // which is what this bitwise comparison is here to catch.
      expectFloatMatches(out[i], src[i] / 255.0f, kUnitRangeTolerance);
    }
  }
}

TEST(FloatPixmapConversionTest, FromPixmapCoversEveryByteValue) {
  std::vector<std::uint8_t> bytes(256);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }

  const Pixmap source = *Pixmap::fromVec(std::move(bytes), IntSize::fromWH(8, 8).value());
  const FloatPixmap converted = FloatPixmap::fromPixmap(source);

  const auto src = source.data();
  const auto out = converted.data();
  for (std::size_t i = 0; i < src.size(); ++i) {
    SCOPED_TRACE(testing::Message() << "byte=" << int{src[i]});
    expectFloatMatches(out[i], src[i] / 255.0f, kUnitRangeTolerance);
  }
}

TEST(FloatPixmapConversionTest, ToPixmapMatchesScalarClampAndTruncate) {
  // Values below zero and above one exercise both clamp arms; the rest sit on
  // and beside the rounding boundaries the +0.5 truncation turns over.
  static const std::array<float, 12> kValues = {
      -1.0f, -0.5f,         -0.0f,           0.0f, 1.0f / 512.0f, 1.0f / 510.0f,
      0.5f,  1.0f / 255.0f, 254.0f / 255.0f, 1.0f, 1.5f,          2.0f};

  // 7 pixels is 28 floats: one full 16-float vector step plus a 12-float tail.
  FloatPixmap pixmap = *FloatPixmap::fromSize(7, 1);
  auto data = pixmap.data();
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = kValues[i % kValues.size()];
  }

  const Pixmap converted = pixmap.toPixmap();
  const auto out = converted.data();
  ASSERT_EQ(out.size(), data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    SCOPED_TRACE(testing::Message() << "i=" << i << " value=" << data[i]);
    const float scaled = data[i] * 255.0f + 0.5f;
    const float clamped = scaled < 0.0f ? 0.0f : (255.0f < scaled ? 255.0f : scaled);
    EXPECT_EQ(int{out[i]}, int{static_cast<std::uint8_t>(clamped)});
  }
}

TEST(FloatPixmapConversionTest, ToPixmapMapsNanLanesToZeroBytes) {
  if (!kVectorToPixmap) {
    GTEST_SKIP() << "the scalar fallback's cast of a NaN float to uint8 is undefined behaviour";
  }

  // The clamp in the vector conversion is a nested selection, so a NaN lane
  // falls through both comparisons unchanged, exactly as `std::clamp` does.
  // Each conversion then turns that NaN into a zero byte: the wasm128
  // saturating conversion defines NaN as zero, and the x86 conversion produces
  // INT_MIN, which the signed pack clamps to -32768 and the unsigned pack
  // clamps to 0. A clamp whose operands were the other way round would select
  // the bound the scalar formula rejects and store 255 instead, so this is the
  // expectation that holds that operand order in place.
  const float nan = quietNan();

  // Four pixels is exactly one 16-float vector step, so no lane reaches the
  // scalar tail. A NaN sits in a different position in each group of four so a
  // lane mix-up cannot hide one.
  static const std::array<float, 16> kValues = {nan,   0.0f,  1.0f,  0.5f,    //
                                                0.25f, nan,   0.75f, 1.0f,    //
                                                2.0f,  -1.0f, nan,   0.125f,  //
                                                1.0f,  0.5f,  0.0f,  nan};

  FloatPixmap pixmap = *FloatPixmap::fromSize(4, 1);
  auto data = pixmap.data();
  ASSERT_EQ(data.size(), kValues.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = kValues[i];
  }

  const Pixmap converted = pixmap.toPixmap();
  const auto out = converted.data();
  ASSERT_EQ(out.size(), kValues.size());
  for (std::size_t i = 0; i < kValues.size(); ++i) {
    SCOPED_TRACE(testing::Message() << "i=" << i << " value=" << kValues[i]);
    if (std::isnan(kValues[i])) {
      EXPECT_EQ(int{out[i]}, 0);
      continue;
    }

    const float scaled = kValues[i] * 255.0f + 0.5f;
    const float clamped = scaled < 0.0f ? 0.0f : (255.0f < scaled ? 255.0f : scaled);
    EXPECT_EQ(int{out[i]}, int{static_cast<std::uint8_t>(clamped)});
  }
}

TEST(FloatPixmapConversionTest, RoundTripPreservesEveryByteValue) {
  std::vector<std::uint8_t> bytes(256);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }

  const Pixmap source = *Pixmap::fromVec(bytes, IntSize::fromWH(8, 8).value());
  const Pixmap roundTripped = FloatPixmap::fromPixmap(source).toPixmap();
  EXPECT_THAT(roundTripped.data(), ElementsAreArray(bytes));
}

// ---------------------------------------------------------------------------
// ColorMatrix (float)
// ---------------------------------------------------------------------------

/// Longhand scalar reference for one premultiplied float pixel: unpremultiply,
/// apply the 5x4 matrix left to right, clamp, then re-premultiply.
std::array<float, 4> referenceColorMatrixPixel(const std::array<float, 4>& pixel,
                                               const std::array<double, 20>& matrix) {
  float m[20];
  for (int j = 0; j < 20; ++j) {
    m[j] = static_cast<float>(matrix[j]);
  }

  const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

  const float pa = pixel[3];
  if (pa == 0.0f) {
    const float ca = clamp01(m[19]);
    if (ca == 0.0f) {
      return pixel;
    }
    return {clamp01(m[4] * ca), clamp01(m[9] * ca), clamp01(m[14] * ca), ca};
  }

  const float invAlpha = 1.0f / pa;
  const float r = pixel[0] * invAlpha;
  const float g = pixel[1] * invAlpha;
  const float b = pixel[2] * invAlpha;

  const float nr = m[0] * r + m[1] * g + m[2] * b + m[3] * pa + m[4];
  const float ng = m[5] * r + m[6] * g + m[7] * b + m[8] * pa + m[9];
  const float nb = m[10] * r + m[11] * g + m[12] * b + m[13] * pa + m[14];
  const float na = m[15] * r + m[16] * g + m[17] * b + m[18] * pa + m[19];

  const float ca = clamp01(na);
  return {clamp01(clamp01(nr) * ca), clamp01(clamp01(ng) * ca), clamp01(clamp01(nb) * ca), ca};
}

/// Premultiplied sample pixels: every color channel stays at or below alpha, as
/// a premultiplied buffer guarantees.
const std::array<std::array<float, 4>, 8>& colorMatrixSamplePixels() {
  static const std::array<std::array<float, 4>, 8> kPixels = {{
      {{0.0f, 0.0f, 0.0f, 0.0f}},
      {{0.0f, 0.0f, 0.0f, 1.0f}},
      {{1.0f, 1.0f, 1.0f, 1.0f}},
      {{0.25f, 0.5f, 0.75f, 1.0f}},
      {{0.1f, 0.05f, 0.02f, 0.125f}},
      {{0.5f, 0.0f, 0.5f, 0.5f}},
      {{0.003921569f, 0.0f, 0.0f, 0.003921569f}},
      {{0.9f, 0.3f, 0.6f, 0.95f}},
  }};
  return kPixels;
}

const std::array<std::array<double, 20>, 5>& colorMatrixSampleMatrices() {
  // clang-format off
  static const std::array<std::array<double, 20>, 5> kMatrices = {{
      identityMatrix(),
      saturateMatrix(0.3),
      hueRotateMatrix(45.0),
      luminanceToAlphaMatrix(),
      // Deliberately extreme: negative and greater-than-one coefficients plus
      // translations, so both clamp arms are reached on every channel.
      {{ 1.7, -0.4,  0.2, 0.0,  0.3,
        -0.6,  1.2,  0.5, 0.1, -0.2,
         0.3,  0.3,  1.4, 0.0,  0.25,
         0.2, -0.1,  0.4, 0.9, -0.15}},
  }};
  // clang-format on
  return kMatrices;
}

TEST(ColorMatrixFloatTest, MatchesScalarReferencePerPixel) {
  for (std::size_t matrixIndex = 0; matrixIndex < colorMatrixSampleMatrices().size();
       ++matrixIndex) {
    SCOPED_TRACE(testing::Message() << "matrix=" << matrixIndex);
    const std::array<double, 20>& matrix = colorMatrixSampleMatrices()[matrixIndex];

    const auto& pixels = colorMatrixSamplePixels();
    FloatPixmap pixmap = *FloatPixmap::fromSize(static_cast<std::uint32_t>(pixels.size()), 1);
    auto data = pixmap.data();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      for (std::size_t channel = 0; channel < 4; ++channel) {
        data[i * 4 + channel] = pixels[i][channel];
      }
    }

    colorMatrix(pixmap, matrix);

    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const std::array<float, 4> expected = referenceColorMatrixPixel(pixels[i], matrix);
      for (std::size_t channel = 0; channel < 4; ++channel) {
        SCOPED_TRACE(testing::Message() << "pixel=" << i << " channel=" << channel);
        expectFloatMatches(data[i * 4 + channel], expected[channel], kUnitRangeTolerance);
      }
    }
  }
}

TEST(ColorMatrixFloatTest, TranslationOnlyMatrixReachesTransparentPixels) {
  // A fully transparent pixel takes the scalar shortcut in every branch, so
  // this pins the shortcut rather than the vector arithmetic.
  const std::array<double, 20> matrix = {0, 0, 0, 0, 0.75,  //
                                         0, 0, 0, 0, 0.25,  //
                                         0, 0, 0, 0, 1.5,   //
                                         0, 0, 0, 0, 0.5};

  const std::array<float, 4> transparent = {0.0f, 0.0f, 0.0f, 0.0f};
  FloatPixmap pixmap = *FloatPixmap::fromSize(1, 1);
  colorMatrix(pixmap, matrix);

  const std::array<float, 4> expected = referenceColorMatrixPixel(transparent, matrix);
  for (std::size_t channel = 0; channel < 4; ++channel) {
    SCOPED_TRACE(testing::Message() << "channel=" << channel);
    expectFloatMatches(pixmap.data()[channel], expected[channel], kUnitRangeTolerance);
  }
}

TEST(ColorMatrixFloatTest, DenormalAlphaProducesNanThatTheClampPassesThrough) {
  // This is how NaN reaches the clamp in ordinary use. A premultiplied pixel
  // whose alpha is denormal makes the unpremultiply's reciprocal overflow to
  // infinity, and a zero color channel times that infinity is NaN, which then
  // spreads across every product and sum in the matrix.
  //
  // Every branch's clamp is a nested selection, so the NaN falls through both
  // comparisons and survives, exactly as `std::clamp` leaves it. A clamp whose
  // operands were the other way round would select a bound instead and store 0
  // or 1, so this is the expectation that holds that operand order in place.
  FloatPixmap pixmap = *FloatPixmap::fromSize(1, 1);
  auto data = pixmap.data();
  data[0] = 0.0f;
  data[1] = 0.0f;
  data[2] = 0.0f;
  data[3] = std::numeric_limits<float>::denorm_min();

  // Guards the premise: a platform that computed a finite reciprocal here would
  // never reach the clamp with a NaN and would make the expectations below
  // vacuous.
  ASSERT_TRUE(std::isinf(1.0f / data[3]));

  colorMatrix(pixmap, identityMatrix());

  const auto out = pixmap.data();
  for (std::size_t channel = 0; channel < 4; ++channel) {
    SCOPED_TRACE(testing::Message() << "channel=" << channel);
    EXPECT_TRUE(std::isnan(out[channel])) << "value=" << out[channel];
  }
}

// ---------------------------------------------------------------------------
// Merge (uint8)
// ---------------------------------------------------------------------------

/// Longhand scalar Source Over reference for the 8-bit merge.
std::vector<std::uint8_t> referenceMerge(const std::vector<std::vector<std::uint8_t>>& layers,
                                         std::size_t byteCount) {
  std::vector<std::uint8_t> out(byteCount, 0);

  for (const std::vector<std::uint8_t>& layer : layers) {
    for (std::size_t off = 0; off + 4 <= byteCount; off += 4) {
      const std::uint32_t sa = layer[off + 3];
      if (sa == 0) {
        continue;
      }
      if (sa == 255) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
          out[off + channel] = layer[off + channel];
        }
        continue;
      }

      const std::uint32_t invSa = 255 - sa;
      for (std::size_t channel = 0; channel < 4; ++channel) {
        const std::uint32_t product = std::uint32_t{out[off + channel]} * invSa;
        const std::uint32_t scaled = (product + 128 + ((product + 128) >> 8)) >> 8;
        const std::uint32_t sum = std::uint32_t{layer[off + channel]} + scaled;
        out[off + channel] = static_cast<std::uint8_t>(sum < 255u ? sum : 255u);
      }
    }
  }

  return out;
}

/// Builds a premultiplied layer: every color channel stays at or below alpha,
/// which is what the vector path's dropped `srcAlpha == 0` shortcut relies on.
std::vector<std::uint8_t> makeLayerBytes(std::size_t pixelCount, std::size_t seed) {
  std::vector<std::uint8_t> bytes(pixelCount * 4);
  for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
    const std::uint8_t alpha = sampleByte(pixel * 5 + seed);
    for (std::size_t channel = 0; channel < 3; ++channel) {
      const std::uint8_t raw = sampleByte(pixel * 4 + channel + seed * 7);
      bytes[pixel * 4 + channel] = static_cast<std::uint8_t>(raw <= alpha ? raw : alpha);
    }
    bytes[pixel * 4 + 3] = alpha;
  }

  return bytes;
}

TEST(MergeUint8Test, MatchesScalarReferenceAcrossVectorStepAndTail) {
  // Pixel counts sweep the 4-pixel vector step and every tail remainder.
  for (std::size_t pixelCount = 1; pixelCount <= 11; ++pixelCount) {
    SCOPED_TRACE(testing::Message() << "pixelCount=" << pixelCount);

    const std::vector<std::vector<std::uint8_t>> layerBytes = {makeLayerBytes(pixelCount, 1),
                                                               makeLayerBytes(pixelCount, 29),
                                                               makeLayerBytes(pixelCount, 71)};

    const IntSize size = IntSize::fromWH(static_cast<std::uint32_t>(pixelCount), 1).value();
    std::vector<Pixmap> pixmaps;
    for (const std::vector<std::uint8_t>& bytes : layerBytes) {
      pixmaps.push_back(*Pixmap::fromVec(bytes, size));
    }

    std::vector<const Pixmap*> layers;
    for (const Pixmap& pixmap : pixmaps) {
      layers.push_back(&pixmap);
    }

    Pixmap dst = *Pixmap::fromSize(static_cast<std::uint32_t>(pixelCount), 1);
    merge(std::span<const Pixmap* const>(layers), dst);

    EXPECT_THAT(dst.data(), ElementsAreArray(referenceMerge(layerBytes, pixelCount * 4)));
  }
}

TEST(MergeUint8Test, SaturatesAndRoundsAtTheExtremes) {
  // Opaque white under a nearly transparent white saturates the sum, and the
  // low alphas land on div255 rounding boundaries. Four pixels is exactly one
  // vector step, so this runs entirely through the vector path where present.
  const std::vector<std::uint8_t> bottom = {255, 255, 255, 255, 128, 128, 128, 128,
                                            1,   1,   1,   1,   254, 254, 254, 254};
  const std::vector<std::uint8_t> top = {1, 1, 1, 1, 255, 255, 255, 255,
                                         2, 2, 2, 2, 128, 0,   64,  128};

  const IntSize size = IntSize::fromWH(4, 1).value();
  const Pixmap bottomPixmap = *Pixmap::fromVec(bottom, size);
  const Pixmap topPixmap = *Pixmap::fromVec(top, size);
  const std::array<const Pixmap*, 2> layers = {&bottomPixmap, &topPixmap};

  Pixmap dst = *Pixmap::fromSize(4, 1);
  merge(std::span<const Pixmap* const>(layers), dst);

  EXPECT_THAT(dst.data(), ElementsAreArray(referenceMerge({bottom, top}, bottom.size())));
}

// ---------------------------------------------------------------------------
// Turbulence lattice blend
// ---------------------------------------------------------------------------

using Gradient = std::array<float, 4>;

/// Longhand scalar reference for `turbulenceBlend4`, one channel at a time.
Gradient referenceTurbulenceBlend(const TurbulenceCorners& corners,
                                  const TurbulenceWeights& weights) {
  Gradient out{};
  for (std::size_t ch = 0; ch < 4; ++ch) {
    float u = corners.gradientX00[ch] * weights.rx0 + corners.gradientY00[ch] * weights.ry0;
    float v = corners.gradientX10[ch] * weights.rx1 + corners.gradientY10[ch] * weights.ry0;
    const float top = u + weights.sx * (v - u);

    u = corners.gradientX01[ch] * weights.rx0 + corners.gradientY01[ch] * weights.ry1;
    v = corners.gradientX11[ch] * weights.rx1 + corners.gradientY11[ch] * weights.ry1;
    const float bottom = u + weights.sx * (v - u);

    out[ch] = top + weights.sy * (bottom - top);
  }

  return out;
}

TEST(TurbulenceBlendTest, MatchesScalarReference) {
  // Gradient components are unit-length pairs in the generator, so every value
  // here is in [-1, 1]; the deliberately asymmetric per-corner and per-channel
  // values make any corner or lane mix-up visible.
  alignas(16) static const std::array<Gradient, 8> kGradients = {{
      {{1.0f, -1.0f, 0.5f, -0.25f}},
      {{-0.125f, 0.75f, -0.875f, 0.0f}},
      {{0.3125f, 0.0625f, -0.5f, 1.0f}},
      {{-1.0f, -0.5f, 0.25f, 0.125f}},
      {{0.9375f, -0.03125f, 0.6875f, -0.4375f}},
      {{-0.75f, 0.5f, 0.0f, -0.9375f}},
      {{0.15625f, -0.84375f, 0.34375f, 0.71875f}},
      {{-0.28125f, 0.96875f, -0.65625f, 0.09375f}},
  }};

  const TurbulenceCorners corners{kGradients[0].data(), kGradients[1].data(), kGradients[2].data(),
                                  kGradients[3].data(), kGradients[4].data(), kGradients[5].data(),
                                  kGradients[6].data(), kGradients[7].data()};

  // rx1 is rx0 - 1 and sx is the s-curve of rx0, exactly as the caller derives
  // them, so these are the shapes the kernel actually sees.
  for (float rx0 : {0.0f, 0.125f, 0.5f, 0.75f, 0.999f}) {
    for (float ry0 : {0.0f, 0.25f, 0.5f, 0.875f}) {
      SCOPED_TRACE(testing::Message() << "rx0=" << rx0 << " ry0=" << ry0);

      const TurbulenceWeights weights{rx0,
                                      rx0 - 1.0f,
                                      ry0,
                                      ry0 - 1.0f,
                                      rx0 * rx0 * (3.0f - 2.0f * rx0),
                                      ry0 * ry0 * (3.0f - 2.0f * ry0)};

      Gradient actual{};
      turbulenceBlend4(corners, weights, actual.data());
      const Gradient expected = referenceTurbulenceBlend(corners, weights);

      for (std::size_t ch = 0; ch < 4; ++ch) {
        SCOPED_TRACE(testing::Message() << "channel=" << ch);
        expectFloatMatches(actual[ch], expected[ch], kUnitRangeTolerance);
      }
    }
  }
}

TEST(TurbulenceBlendTest, IsWiredIntoTheFloatGeneratorConsistentlyWithTheDoublePath) {
  // The float generator and the uint8 generator share the same permutation and
  // gradient tables and differ only in precision, so their outputs agree to
  // within 8-bit quantization. A corner or channel mix-up inside the vector
  // kernel would move the noise field far outside that band, which is what
  // this covers that the kernel-level test cannot.
  TurbulenceParams params;
  params.type = TurbulenceType::Turbulence;
  params.baseFrequencyX = 0.2;
  params.baseFrequencyY = 0.15;
  params.numOctaves = 2;
  params.seed = 7.0;

  Pixmap bytePixmap = *Pixmap::fromSize(9, 5);
  turbulence(bytePixmap, params);

  FloatPixmap floatPixmap = *FloatPixmap::fromSize(9, 5);
  turbulence(floatPixmap, params);

  const auto byteData = bytePixmap.data();
  const auto floatData = floatPixmap.data();
  ASSERT_EQ(byteData.size(), floatData.size());
  for (std::size_t i = 0; i < byteData.size(); ++i) {
    SCOPED_TRACE(testing::Message() << "i=" << i);
    // Two byte steps covers the double-to-float drift plus the uint8 path's own
    // rounding of the premultiply.
    EXPECT_NEAR(floatData[i] * 255.0f, static_cast<float>(byteData[i]), 2.0f);
  }
}

}  // namespace
}  // namespace tiny_skia::filter
