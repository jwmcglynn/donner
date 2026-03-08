#include "tiny_skia/filter/Blend.h"

#include <algorithm>
#include <cstddef>

namespace tiny_skia::filter {

namespace {

// Blend functions operate on unpremultiplied values in [0, 1].
inline double blendNormal(double, double cs) {
  return cs;
}

inline double blendMultiply(double cb, double cs) {
  return cb * cs;
}

inline double blendScreen(double cb, double cs) {
  return cb + cs - cb * cs;
}

inline double blendDarken(double cb, double cs) {
  return std::min(cb, cs);
}

inline double blendLighten(double cb, double cs) {
  return std::max(cb, cs);
}

}  // namespace

void blend(const Pixmap& bg, const Pixmap& fg, Pixmap& dst, BlendMode mode) {
  const auto bgData = bg.data();
  const auto fgData = fg.data();
  auto dstData = dst.data();
  const std::size_t pixelCount = dstData.size() / 4;

  // Select blend function.
  double (*blendFn)(double, double) = blendNormal;
  switch (mode) {
    case BlendMode::Normal: blendFn = blendNormal; break;
    case BlendMode::Multiply: blendFn = blendMultiply; break;
    case BlendMode::Screen: blendFn = blendScreen; break;
    case BlendMode::Darken: blendFn = blendDarken; break;
    case BlendMode::Lighten: blendFn = blendLighten; break;
  }

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;

    // Read premultiplied RGBA.
    const double pBgR = bgData[off + 0];
    const double pBgG = bgData[off + 1];
    const double pBgB = bgData[off + 2];
    const double bgA = bgData[off + 3] / 255.0;

    const double pFgR = fgData[off + 0];
    const double pFgG = fgData[off + 1];
    const double pFgB = fgData[off + 2];
    const double fgA = fgData[off + 3] / 255.0;

    // Result alpha: Ar = As + Ab - As*Ab (Source Over compositing).
    const double resultA = fgA + bgA - fgA * bgA;

    if (resultA == 0.0) {
      dstData[off + 0] = 0;
      dstData[off + 1] = 0;
      dstData[off + 2] = 0;
      dstData[off + 3] = 0;
      continue;
    }

    // Unpremultiply for blend calculation.
    const double bgR = bgA > 0 ? pBgR / (bgA * 255.0) : 0.0;
    const double bgG = bgA > 0 ? pBgG / (bgA * 255.0) : 0.0;
    const double bgB = bgA > 0 ? pBgB / (bgA * 255.0) : 0.0;
    const double fgR = fgA > 0 ? pFgR / (fgA * 255.0) : 0.0;
    const double fgG = fgA > 0 ? pFgG / (fgA * 255.0) : 0.0;
    const double fgB = fgA > 0 ? pFgB / (fgA * 255.0) : 0.0;

    // CSS compositing formula:
    // Co = (1-Ab)*Cs + (1-As)*Cb + As*Ab*B(Cb,Cs)
    // Then Cr = Co / Ar (since Co is premultiplied by Ar conceptually)
    // But the standard formula gives the premultiplied result directly:
    // Premultiplied: Co = (1-Ab)*As*Cs + (1-As)*Ab*Cb + As*Ab*B(Cb,Cs)
    auto blendChannel = [&](double cb, double cs) -> double {
      const double blended = blendFn(cb, cs);
      const double co = (1.0 - bgA) * fgA * cs + (1.0 - fgA) * bgA * cb + fgA * bgA * blended;
      return std::clamp(co * 255.0, 0.0, 255.0);
    };

    dstData[off + 0] = static_cast<std::uint8_t>(std::round(blendChannel(bgR, fgR)));
    dstData[off + 1] = static_cast<std::uint8_t>(std::round(blendChannel(bgG, fgG)));
    dstData[off + 2] = static_cast<std::uint8_t>(std::round(blendChannel(bgB, fgB)));
    dstData[off + 3] = static_cast<std::uint8_t>(std::round(std::clamp(resultA * 255.0, 0.0, 255.0)));
  }
}

}  // namespace tiny_skia::filter
