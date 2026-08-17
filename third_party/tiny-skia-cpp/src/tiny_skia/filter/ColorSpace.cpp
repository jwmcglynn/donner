#include "tiny_skia/filter/ColorSpace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace tiny_skia::filter {

namespace {

/// Exact sRGB -> linear transfer function, computed in double precision.
/// C_linear = C_srgb <= 0.04045 ? C_srgb/12.92 : pow((C_srgb+0.055)/1.055, 2.4)
double srgbToLinearExact(double s) {
  if (s <= 0.04045) {
    return s / 12.92;
  }
  return std::pow((s + 0.055) / 1.055, 2.4);
}

/// Exact linear -> sRGB transfer function, computed in double precision.
/// C_srgb = C_linear <= 0.0031308 ? 12.92*C_linear : 1.055*pow(C_linear, 1/2.4)-0.055
double linearToSrgbExact(double l) {
  if (l <= 0.0031308) {
    return 12.92 * l;
  }
  return 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

// Pre-computed LUT for sRGB -> linear (256 entries, input is 8-bit sRGB value).
// Exact for every possible input byte: each entry is the double-precision
// transfer function evaluated at i/255.
std::array<double, 256> buildSrgbToLinearLut() {
  std::array<double, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    lut[i] = srgbToLinearExact(i / 255.0);
  }
  return lut;
}

// Pre-computed LUT for linear -> sRGB (4096 entries for precision).
std::array<std::uint8_t, 4096> buildLinearToSrgbLut() {
  std::array<std::uint8_t, 4096> lut{};
  for (int i = 0; i < 4096; ++i) {
    const double s = linearToSrgbExact(i / 4095.0);
    lut[i] = static_cast<std::uint8_t>(std::clamp(std::round(s * 255.0), 0.0, 255.0));
  }
  return lut;
}

const auto& srgbToLinearLut() {
  static const auto lut = buildSrgbToLinearLut();
  return lut;
}

const auto& linearToSrgbLut() {
  static const auto lut = buildLinearToSrgbLut();
  return lut;
}

// Float-path LUTs for the FloatPixmap conversions: 4096 entries over [0,1],
// direct-indexed with rounding (index = round(x * 4095), input clamped to
// [0,1] first). Entries are the exact double-precision transfer functions
// sampled at i/4095, so the tables cover both the linear toe segment and the
// power segment without a per-pixel branch.
//
// Accuracy, measured over a 2^24-point uniform sweep of [0,1] with deltas
// expressed in 8-bit output units (1 unit = 1/255):
//   sRGB -> linear: max |LUT - exact| = 0.071 units;
//                   max |LUT - previous approxPowf path| = 0.121 units.
//   linear -> sRGB: max |LUT - exact| = 0.402 units;
//                   max |LUT - previous approxPowf path| = 0.402 units.
// Both directions stay well within 1 unit of the previous per-pixel
// approxPowf() implementation, so a rounded 8-bit result differs by at most
// 1. The linear -> sRGB bound is set by table quantization at the dark end,
// where the transfer slope approaches 12.92; 4096 entries keep that below
// half a unit. ColorSpaceTest.cpp locks in these bounds.
constexpr int kFloatLutSize = 4096;

std::array<float, kFloatLutSize> buildSrgbToLinearFloatLut() {
  std::array<float, kFloatLutSize> lut{};
  for (int i = 0; i < kFloatLutSize; ++i) {
    lut[i] = static_cast<float>(srgbToLinearExact(static_cast<double>(i) / (kFloatLutSize - 1)));
  }
  return lut;
}

std::array<float, kFloatLutSize> buildLinearToSrgbFloatLut() {
  std::array<float, kFloatLutSize> lut{};
  for (int i = 0; i < kFloatLutSize; ++i) {
    lut[i] = static_cast<float>(linearToSrgbExact(static_cast<double>(i) / (kFloatLutSize - 1)));
  }
  return lut;
}

// Lazily built on first use. C++11 function-local static initialization is
// guaranteed thread-safe, so concurrent first callers are fine.
const std::array<float, kFloatLutSize>& srgbToLinearFloatLut() {
  static const auto lut = buildSrgbToLinearFloatLut();
  return lut;
}

const std::array<float, kFloatLutSize>& linearToSrgbFloatLut() {
  static const auto lut = buildLinearToSrgbFloatLut();
  return lut;
}

/// Looks up a unit-range value in a float LUT, clamping the input to [0,1].
inline float lookupUnit(const std::array<float, kFloatLutSize>& lut, float x) {
  const float clamped = std::clamp(x, 0.0f, 1.0f);
  return lut[static_cast<int>(clamped * (kFloatLutSize - 1) + 0.5f)];
}

}  // namespace

void srgbToLinear(Pixmap& pixmap) {
  const auto& lut = srgbToLinearLut();
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const std::uint8_t a = data[off + 3];

    if (a == 0) {
      continue;
    }

    if (a == 255) {
      // Fully opaque: direct LUT lookup, re-premultiply is identity.
      const double lr = lut[data[off + 0]];
      const double lg = lut[data[off + 1]];
      const double lb = lut[data[off + 2]];
      data[off + 0] = static_cast<std::uint8_t>(std::clamp(std::round(lr * 255.0), 0.0, 255.0));
      data[off + 1] = static_cast<std::uint8_t>(std::clamp(std::round(lg * 255.0), 0.0, 255.0));
      data[off + 2] = static_cast<std::uint8_t>(std::clamp(std::round(lb * 255.0), 0.0, 255.0));
    } else {
      // Unpremultiply, convert, re-premultiply.
      const double invAlpha = 255.0 / a;
      const double alphaFrac = a / 255.0;

      // Unpremultiply to get sRGB values [0, 255].
      const std::uint8_t sr =
          static_cast<std::uint8_t>(std::clamp(std::round(data[off + 0] * invAlpha), 0.0, 255.0));
      const std::uint8_t sg =
          static_cast<std::uint8_t>(std::clamp(std::round(data[off + 1] * invAlpha), 0.0, 255.0));
      const std::uint8_t sb =
          static_cast<std::uint8_t>(std::clamp(std::round(data[off + 2] * invAlpha), 0.0, 255.0));

      // Convert sRGB -> linear using LUT.
      const double lr = lut[sr];
      const double lg = lut[sg];
      const double lb = lut[sb];

      // Re-premultiply with alpha.
      data[off + 0] =
          static_cast<std::uint8_t>(std::clamp(std::round(lr * alphaFrac * 255.0), 0.0, 255.0));
      data[off + 1] =
          static_cast<std::uint8_t>(std::clamp(std::round(lg * alphaFrac * 255.0), 0.0, 255.0));
      data[off + 2] =
          static_cast<std::uint8_t>(std::clamp(std::round(lb * alphaFrac * 255.0), 0.0, 255.0));
    }
    // Alpha is unchanged.
  }
}

void linearToSrgb(Pixmap& pixmap) {
  const auto& lut = linearToSrgbLut();
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const std::uint8_t a = data[off + 3];

    if (a == 0) {
      continue;
    }

    if (a == 255) {
      // Fully opaque: convert linear [0, 255] -> LUT index [0, 4095].
      const int ir = static_cast<int>(std::round(data[off + 0] * 4095.0 / 255.0));
      const int ig = static_cast<int>(std::round(data[off + 1] * 4095.0 / 255.0));
      const int ib = static_cast<int>(std::round(data[off + 2] * 4095.0 / 255.0));
      data[off + 0] = lut[std::clamp(ir, 0, 4095)];
      data[off + 1] = lut[std::clamp(ig, 0, 4095)];
      data[off + 2] = lut[std::clamp(ib, 0, 4095)];
    } else {
      // Unpremultiply, convert, re-premultiply.
      const double invAlpha = 255.0 / a;
      const double alphaFrac = a / 255.0;

      // Unpremultiply to get linear values [0, 255].
      const double lr = std::clamp(data[off + 0] * invAlpha, 0.0, 255.0);
      const double lg = std::clamp(data[off + 1] * invAlpha, 0.0, 255.0);
      const double lb = std::clamp(data[off + 2] * invAlpha, 0.0, 255.0);

      // Convert linear [0, 255] -> LUT index [0, 4095].
      const int ir = static_cast<int>(std::round(lr * 4095.0 / 255.0));
      const int ig = static_cast<int>(std::round(lg * 4095.0 / 255.0));
      const int ib = static_cast<int>(std::round(lb * 4095.0 / 255.0));

      // Look up sRGB values and re-premultiply.
      data[off + 0] = static_cast<std::uint8_t>(
          std::clamp(std::round(lut[std::clamp(ir, 0, 4095)] * alphaFrac), 0.0, 255.0));
      data[off + 1] = static_cast<std::uint8_t>(
          std::clamp(std::round(lut[std::clamp(ig, 0, 4095)] * alphaFrac), 0.0, 255.0));
      data[off + 2] = static_cast<std::uint8_t>(
          std::clamp(std::round(lut[std::clamp(ib, 0, 4095)] * alphaFrac), 0.0, 255.0));
    }
    // Alpha is unchanged.
  }
}

void srgbToLinear(FloatPixmap& pixmap) {
  // sRGB transfer function (inverse gamma) via direct-indexed LUT. Replaces a
  // per-channel approxPowf() evaluation, which profiling showed dominating
  // filter scenes that convert to linearRGB and back around every primitive.
  const auto& lut = srgbToLinearFloatLut();
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const float a = data[off + 3];

    if (a <= 0.0f) {
      continue;
    }

    if (a >= 1.0f) {
      // Fully opaque: direct conversion.
      data[off + 0] = lookupUnit(lut, data[off + 0]);
      data[off + 1] = lookupUnit(lut, data[off + 1]);
      data[off + 2] = lookupUnit(lut, data[off + 2]);
    } else {
      // Unpremultiply, convert, re-premultiply.
      const float invAlpha = 1.0f / a;

      data[off + 0] = lookupUnit(lut, data[off + 0] * invAlpha) * a;
      data[off + 1] = lookupUnit(lut, data[off + 1] * invAlpha) * a;
      data[off + 2] = lookupUnit(lut, data[off + 2] * invAlpha) * a;
    }
  }
}

void linearToSrgb(FloatPixmap& pixmap) {
  // sRGB transfer function (apply gamma) via direct-indexed LUT; see
  // srgbToLinear() above for why the approxPowf() path was replaced.
  const auto& lut = linearToSrgbFloatLut();
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const float a = data[off + 3];

    if (a <= 0.0f) {
      continue;
    }

    if (a >= 1.0f) {
      data[off + 0] = lookupUnit(lut, data[off + 0]);
      data[off + 1] = lookupUnit(lut, data[off + 1]);
      data[off + 2] = lookupUnit(lut, data[off + 2]);
    } else {
      const float invAlpha = 1.0f / a;

      data[off + 0] = lookupUnit(lut, data[off + 0] * invAlpha) * a;
      data[off + 1] = lookupUnit(lut, data[off + 1] * invAlpha) * a;
      data[off + 2] = lookupUnit(lut, data[off + 2] * invAlpha) * a;
    }
  }
}

}  // namespace tiny_skia::filter
