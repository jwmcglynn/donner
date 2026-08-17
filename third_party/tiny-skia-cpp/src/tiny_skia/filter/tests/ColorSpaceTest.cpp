/// Tests for tiny_skia/filter/ColorSpace.
///
/// The conversions are implemented with lookup tables:
///   - uint8 sRGB -> linear uses a 256-entry table that must be EXACT for
///     every possible input byte (each entry is the double-precision transfer
///     function evaluated at i/255).
///   - The float-path conversions use 4096-entry tables over [0,1]. The
///     measured accuracy bounds (see ColorSpace.cpp) are locked in here with
///     a dense sweep: sRGB -> linear within 0.2/255 of the exact transfer
///     function, linear -> sRGB within 0.45/255. Both are well inside the
///     1/255 budget that keeps rounded 8-bit output within 1 unit of a
///     per-pixel evaluation.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/ColorSpace.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {
namespace {

/// Exact sRGB -> linear transfer function (reference, double precision).
double srgbToLinearExact(double s) {
  if (s <= 0.04045) {
    return s / 12.92;
  }
  return std::pow((s + 0.055) / 1.055, 2.4);
}

/// Exact linear -> sRGB transfer function (reference, double precision).
double linearToSrgbExact(double l) {
  if (l <= 0.0031308) {
    return 12.92 * l;
  }
  return 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

/// Builds the dense sweep used by the float-path bound tests: uniform samples
/// plus the transfer-function breakpoints, the 8-bit grid, and the LUT grid.
std::vector<float> buildDenseSweep() {
  std::vector<float> values;
  constexpr int kUniformSteps = 1 << 20;
  values.reserve(kUniformSteps + 1 + 256 + 4096 + 8);
  for (int i = 0; i <= kUniformSteps; ++i) {
    values.push_back(static_cast<float>(static_cast<double>(i) / kUniformSteps));
  }
  for (int i = 0; i < 256; ++i) {
    values.push_back(static_cast<float>(i) / 255.0f);
  }
  for (int i = 0; i < 4096; ++i) {
    values.push_back(static_cast<float>(i) / 4095.0f);
  }
  for (const float v : {0.0f, 1.0f, 0.04045f, 0.040451f, 0.0031308f, 0.0031309f}) {
    values.push_back(v);
  }
  return values;
}

/// Maximum absolute delta over a sweep, with the input that produced it.
struct SweepResult {
  double maxDelta = 0.0;
  double argmax = 0.0;
};

/// Runs the float-path conversion over `values` (as opaque pixels) and
/// returns the maximum absolute delta against `reference`.
template <typename ConvertFn, typename ReferenceFn>
SweepResult maxDeltaOverSweep(const std::vector<float>& values, ConvertFn convert,
                              ReferenceFn reference) {
  // Pack the sweep into one FloatPixmap (RGB = value, alpha = 1) so the
  // conversion runs through the real pixel loop.
  const auto width = static_cast<std::uint32_t>(std::min<std::size_t>(values.size(), 4096));
  const auto height = static_cast<std::uint32_t>((values.size() + width - 1) / width);
  auto pixmap = FloatPixmap::fromSize(width, height).value();
  auto data = pixmap.data();
  for (std::size_t i = 0; i < values.size(); ++i) {
    data[i * 4 + 0] = values[i];
    data[i * 4 + 1] = values[i];
    data[i * 4 + 2] = values[i];
    data[i * 4 + 3] = 1.0f;
  }
  // Padding pixels stay transparent and are skipped by the conversion.

  convert(pixmap);

  SweepResult result;
  auto converted = pixmap.data();
  for (std::size_t i = 0; i < values.size(); ++i) {
    const double delta = std::abs(static_cast<double>(converted[i * 4]) - reference(values[i]));
    if (delta > result.maxDelta) {
      result.maxDelta = delta;
      result.argmax = values[i];
    }
  }
  return result;
}

TEST(ColorSpaceTest, ByteSrgbToLinearLutIsExactForEveryByte) {
  // 256 opaque pixels, one per possible 8-bit sRGB value.
  auto pixmap = Pixmap::fromSize(256, 1).value();
  auto data = pixmap.data();
  for (int i = 0; i < 256; ++i) {
    const auto v = static_cast<std::uint8_t>(i);
    data[i * 4 + 0] = v;
    data[i * 4 + 1] = v;
    data[i * 4 + 2] = v;
    data[i * 4 + 3] = 255;
  }

  srgbToLinear(pixmap);

  auto converted = pixmap.data();
  for (int i = 0; i < 256; ++i) {
    const auto expected = static_cast<std::uint8_t>(
        std::clamp(std::round(srgbToLinearExact(i / 255.0) * 255.0), 0.0, 255.0));
    EXPECT_EQ(converted[i * 4 + 0], expected) << "sRGB byte " << i;
    EXPECT_EQ(converted[i * 4 + 1], expected) << "sRGB byte " << i;
    EXPECT_EQ(converted[i * 4 + 2], expected) << "sRGB byte " << i;
    EXPECT_EQ(converted[i * 4 + 3], 255) << "alpha must be unchanged, byte " << i;
  }
}

TEST(ColorSpaceTest, FloatSrgbToLinearWithinBoundOfExact) {
  const std::vector<float> sweep = buildDenseSweep();
  const SweepResult result = maxDeltaOverSweep(
      sweep, [](FloatPixmap& p) { srgbToLinear(p); }, [](float s) { return srgbToLinearExact(s); });
  // Measured maximum on a 2^24-point sweep: 0.0708/255.
  EXPECT_LE(result.maxDelta, 0.2 / 255.0)
      << "worst input " << result.argmax << ", delta in 8-bit units " << result.maxDelta * 255.0;
}

TEST(ColorSpaceTest, FloatLinearToSrgbWithinBoundOfExact) {
  const std::vector<float> sweep = buildDenseSweep();
  const SweepResult result = maxDeltaOverSweep(
      sweep, [](FloatPixmap& p) { linearToSrgb(p); }, [](float l) { return linearToSrgbExact(l); });
  // Measured maximum on a 2^24-point sweep: 0.4022/255, set by table
  // quantization at the dark end where the transfer slope approaches 12.92.
  EXPECT_LE(result.maxDelta, 0.45 / 255.0)
      << "worst input " << result.argmax << ", delta in 8-bit units " << result.maxDelta * 255.0;
}

TEST(ColorSpaceTest, DenormalAlphaWithZeroChannelsStaysFiniteAndInRange) {
  // A denormal alpha passes the alpha <= 0 guard while its reciprocal
  // overflows to inf; a zero channel then unpremultiplies to 0 * inf == NaN
  // and a nonzero channel to inf. The table lookup must map those to in-range
  // indices instead of casting NaN to int (undefined behavior; an
  // out-of-bounds table read on x86). Regression coverage for both
  // conversions through the non-opaque branch.
  for (const bool toLinear : {true, false}) {
    auto pixmap = FloatPixmap::fromSize(2, 1).value();
    auto data = pixmap.data();
    constexpr float kDenormal = 1e-45f;  // Smallest positive denormal float.
    // Pixel 0: zero channels, denormal alpha -> 0 * inf == NaN per channel.
    data[0] = 0.0f;
    data[1] = 0.0f;
    data[2] = 0.0f;
    data[3] = kDenormal;
    // Pixel 1: nonzero channels, denormal alpha -> channel * inf == inf.
    data[4] = kDenormal;
    data[5] = 0.5f;
    data[6] = 1.0f;
    data[7] = kDenormal;

    if (toLinear) {
      srgbToLinear(pixmap);
    } else {
      linearToSrgb(pixmap);
    }

    auto converted = pixmap.data();
    for (int pixel = 0; pixel < 2; ++pixel) {
      for (int channel = 0; channel < 3; ++channel) {
        const float value = converted[pixel * 4 + channel];
        EXPECT_TRUE(std::isfinite(value))
            << "pixel " << pixel << " channel " << channel << " toLinear " << toLinear;
        EXPECT_GE(value, 0.0f) << "pixel " << pixel << " channel " << channel;
        EXPECT_LE(value, 1.0f) << "pixel " << pixel << " channel " << channel;
      }
      EXPECT_EQ(converted[pixel * 4 + 3], kDenormal) << "alpha must be unchanged";
    }
  }
}

TEST(ColorSpaceTest, FloatConversionPreservesPremultipliedAlpha) {
  auto pixmap = FloatPixmap::fromSize(2, 1).value();
  auto data = pixmap.data();
  // Pixel 0: premultiplied (0.25, 0.25, 0.25, 0.5), i.e. unpremultiplied 0.5.
  data[0] = 0.25f;
  data[1] = 0.25f;
  data[2] = 0.25f;
  data[3] = 0.5f;
  // Pixel 1: fully transparent, must be untouched.
  data[4] = 0.125f;
  data[5] = 0.125f;
  data[6] = 0.125f;
  data[7] = 0.0f;

  srgbToLinear(pixmap);

  auto converted = pixmap.data();
  const double expected = srgbToLinearExact(0.5) * 0.5;
  EXPECT_NEAR(converted[0], expected, 1.0 / 255.0);
  EXPECT_NEAR(converted[1], expected, 1.0 / 255.0);
  EXPECT_NEAR(converted[2], expected, 1.0 / 255.0);
  EXPECT_FLOAT_EQ(converted[3], 0.5f);

  EXPECT_FLOAT_EQ(converted[4], 0.125f);
  EXPECT_FLOAT_EQ(converted[5], 0.125f);
  EXPECT_FLOAT_EQ(converted[6], 0.125f);
  EXPECT_FLOAT_EQ(converted[7], 0.0f);
}

}  // namespace
}  // namespace tiny_skia::filter
