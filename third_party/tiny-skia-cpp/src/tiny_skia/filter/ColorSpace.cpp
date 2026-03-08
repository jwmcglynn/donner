#include "tiny_skia/filter/ColorSpace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace tiny_skia::filter {

namespace {

// Pre-computed LUT for sRGB -> linear (256 entries, input is 8-bit sRGB value).
std::array<double, 256> buildSrgbToLinearLut() {
  std::array<double, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    const double s = i / 255.0;
    if (s <= 0.04045) {
      lut[i] = s / 12.92;
    } else {
      lut[i] = std::pow((s + 0.055) / 1.055, 2.4);
    }
  }
  return lut;
}

// Pre-computed LUT for linear -> sRGB (4096 entries for precision).
std::array<std::uint8_t, 4096> buildLinearToSrgbLut() {
  std::array<std::uint8_t, 4096> lut{};
  for (int i = 0; i < 4096; ++i) {
    const double l = i / 4095.0;
    double s;
    if (l <= 0.0031308) {
      s = 12.92 * l;
    } else {
      s = 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    }
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

}  // namespace tiny_skia::filter
