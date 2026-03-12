#include "tiny_skia/filter/Blend.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

// --- Separable blend functions (operate on unpremultiplied values in [0, 1]) ---

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

inline double blendOverlay(double cb, double cs) {
  // HardLight with swapped args.
  if (cb <= 0.5) {
    return 2.0 * cb * cs;
  }
  return 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
}

inline double blendColorDodge(double cb, double cs) {
  if (cb == 0.0) {
    return 0.0;
  }
  if (cs == 1.0) {
    return 1.0;
  }
  return std::min(1.0, cb / (1.0 - cs));
}

inline double blendColorBurn(double cb, double cs) {
  if (cb == 1.0) {
    return 1.0;
  }
  if (cs == 0.0) {
    return 0.0;
  }
  return 1.0 - std::min(1.0, (1.0 - cb) / cs);
}

inline double blendHardLight(double cb, double cs) {
  if (cs <= 0.5) {
    return 2.0 * cb * cs;
  }
  return 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
}

inline double blendSoftLight(double cb, double cs) {
  // W3C formula (Pegtop variation).
  if (cs <= 0.5) {
    return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
  }
  double d = (cb <= 0.25) ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : std::sqrt(cb);
  return cb + (2.0 * cs - 1.0) * (d - cb);
}

inline double blendDifference(double cb, double cs) {
  return std::abs(cb - cs);
}

inline double blendExclusion(double cb, double cs) {
  return cb + cs - 2.0 * cb * cs;
}

// --- Non-separable blend mode helpers ---

inline double luminosity(double r, double g, double b) {
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

inline double saturation(double r, double g, double b) {
  return std::max({r, g, b}) - std::min({r, g, b});
}

struct RGB {
  double r, g, b;
};

inline RGB clipColor(double r, double g, double b) {
  double l = luminosity(r, g, b);
  double n = std::min({r, g, b});
  double x = std::max({r, g, b});
  if (n < 0.0) {
    double denom = l - n;
    if (denom != 0.0) {
      r = l + (r - l) * l / denom;
      g = l + (g - l) * l / denom;
      b = l + (b - l) * l / denom;
    }
  }
  if (x > 1.0) {
    double denom = x - l;
    if (denom != 0.0) {
      r = l + (r - l) * (1.0 - l) / denom;
      g = l + (g - l) * (1.0 - l) / denom;
      b = l + (b - l) * (1.0 - l) / denom;
    }
  }
  return {r, g, b};
}

inline RGB setLum(double r, double g, double b, double l) {
  double d = l - luminosity(r, g, b);
  return clipColor(r + d, g + d, b + d);
}

inline RGB setSat(double r, double g, double b, double s) {
  // Sort channels to find min, mid, max.
  double* channels[3] = {&r, &g, &b};
  if (*channels[0] > *channels[1]) {
    std::swap(channels[0], channels[1]);
  }
  if (*channels[1] > *channels[2]) {
    std::swap(channels[1], channels[2]);
  }
  if (*channels[0] > *channels[1]) {
    std::swap(channels[0], channels[1]);
  }
  // channels[0] = min, channels[1] = mid, channels[2] = max
  if (*channels[2] > *channels[0]) {
    *channels[1] = ((*channels[1] - *channels[0]) * s) / (*channels[2] - *channels[0]);
    *channels[2] = s;
  } else {
    *channels[1] = 0.0;
    *channels[2] = 0.0;
  }
  *channels[0] = 0.0;
  return {r, g, b};
}

// Non-separable blend result for a single pixel.
inline RGB blendNonSeparable(BlendMode mode, double cbR, double cbG, double cbB, double csR,
                             double csG, double csB) {
  switch (mode) {
    case BlendMode::Hue: {
      double s = saturation(cbR, cbG, cbB);
      double l = luminosity(cbR, cbG, cbB);
      auto sat = setSat(csR, csG, csB, s);
      return setLum(sat.r, sat.g, sat.b, l);
    }
    case BlendMode::Saturation: {
      double s = saturation(csR, csG, csB);
      double l = luminosity(cbR, cbG, cbB);
      auto sat = setSat(cbR, cbG, cbB, s);
      return setLum(sat.r, sat.g, sat.b, l);
    }
    case BlendMode::Color: {
      double l = luminosity(cbR, cbG, cbB);
      return setLum(csR, csG, csB, l);
    }
    case BlendMode::Luminosity: {
      double l = luminosity(csR, csG, csB);
      return setLum(cbR, cbG, cbB, l);
    }
    default: return {csR, csG, csB};
  }
}

bool isNonSeparable(BlendMode mode) {
  return mode == BlendMode::Hue || mode == BlendMode::Saturation || mode == BlendMode::Color ||
         mode == BlendMode::Luminosity;
}

}  // namespace

void blend(const Pixmap& bg, const Pixmap& fg, Pixmap& dst, BlendMode mode) {
  const auto bgData = bg.data();
  const auto fgData = fg.data();
  auto dstData = dst.data();
  const std::size_t pixelCount = dstData.size() / 4;

  const bool nonSeparable = isNonSeparable(mode);

  // Select separable blend function (nullptr for non-separable).
  double (*blendFn)(double, double) = nullptr;
  if (!nonSeparable) {
    switch (mode) {
      case BlendMode::Normal: blendFn = blendNormal; break;
      case BlendMode::Multiply: blendFn = blendMultiply; break;
      case BlendMode::Screen: blendFn = blendScreen; break;
      case BlendMode::Darken: blendFn = blendDarken; break;
      case BlendMode::Lighten: blendFn = blendLighten; break;
      case BlendMode::Overlay: blendFn = blendOverlay; break;
      case BlendMode::ColorDodge: blendFn = blendColorDodge; break;
      case BlendMode::ColorBurn: blendFn = blendColorBurn; break;
      case BlendMode::HardLight: blendFn = blendHardLight; break;
      case BlendMode::SoftLight: blendFn = blendSoftLight; break;
      case BlendMode::Difference: blendFn = blendDifference; break;
      case BlendMode::Exclusion: blendFn = blendExclusion; break;
      default: blendFn = blendNormal; break;
    }
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

    if (nonSeparable) {
      // Non-separable: compute blended RGB together.
      auto blended = blendNonSeparable(mode, bgR, bgG, bgB, fgR, fgG, fgB);

      // CSS compositing formula (premultiplied output):
      // Co = (1-Ab)*As*Cs + (1-As)*Ab*Cb + As*Ab*B(Cb,Cs)
      auto composite = [&](double cb, double cs, double b) -> double {
        double co = (1.0 - bgA) * fgA * cs + (1.0 - fgA) * bgA * cb + fgA * bgA * b;
        return std::clamp(co * 255.0, 0.0, 255.0);
      };

      dstData[off + 0] =
          static_cast<std::uint8_t>(std::round(composite(bgR, fgR, blended.r)));
      dstData[off + 1] =
          static_cast<std::uint8_t>(std::round(composite(bgG, fgG, blended.g)));
      dstData[off + 2] =
          static_cast<std::uint8_t>(std::round(composite(bgB, fgB, blended.b)));
    } else {
      // Separable: apply blend function per channel.
      // CSS compositing formula:
      // Premultiplied: Co = (1-Ab)*As*Cs + (1-As)*Ab*Cb + As*Ab*B(Cb,Cs)
      auto blendChannel = [&](double cb, double cs) -> double {
        const double blended = blendFn(cb, cs);
        const double co =
            (1.0 - bgA) * fgA * cs + (1.0 - fgA) * bgA * cb + fgA * bgA * blended;
        return std::clamp(co * 255.0, 0.0, 255.0);
      };

      dstData[off + 0] = static_cast<std::uint8_t>(std::round(blendChannel(bgR, fgR)));
      dstData[off + 1] = static_cast<std::uint8_t>(std::round(blendChannel(bgG, fgG)));
      dstData[off + 2] = static_cast<std::uint8_t>(std::round(blendChannel(bgB, fgB)));
    }

    dstData[off + 3] =
        static_cast<std::uint8_t>(std::round(std::clamp(resultA * 255.0, 0.0, 255.0)));
  }
}

void blend(const FloatPixmap& bg, const FloatPixmap& fg, FloatPixmap& dst, BlendMode mode) {
  const auto bgData = bg.data();
  const auto fgData = fg.data();
  auto dstData = dst.data();
  const std::size_t pixelCount = dstData.size() / 4;

  const float* bgPtr = bgData.data();
  const float* fgPtr = fgData.data();
  float* dstPtr = dstData.data();

  // Fast premultiplied paths for modes where the CSS compositing formula simplifies
  // to avoid unpremultiply/premultiply. These use Vec4f32 SIMD for all 4 channels.
  switch (mode) {
    case BlendMode::Normal:
      // Co = pCs + pCb*(1-As) (Source Over compositing)
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 pb = Vec4f32::load(bgPtr + off);
        Vec4f32 pf = Vec4f32::load(fgPtr + off);
        Vec4f32::fmadd(pb, Vec4f32::splat(1.0f - fgPtr[off + 3]), pf).clamp01().store(dstPtr + off);
      }
      return;

    case BlendMode::Multiply:
      // Co = pCs*(1-Ab) + pCb*(1-As) + pCb*pCs
      // Alpha: As + Ab - As*Ab = Source Over (correct for all 4 channels)
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 pb = Vec4f32::load(bgPtr + off);
        Vec4f32 pf = Vec4f32::load(fgPtr + off);
        float bgA = bgPtr[off + 3];
        float fgA = fgPtr[off + 3];
        Vec4f32 result = pf * Vec4f32::splat(1.0f - bgA) + pb * Vec4f32::splat(1.0f - fgA) + pb * pf;
        result.clamp01().store(dstPtr + off);
      }
      return;

    case BlendMode::Screen:
      // Co = pCs + pCb - pCb*pCs
      // Alpha: As + Ab - As*Ab = Source Over (correct for all 4 channels)
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 pb = Vec4f32::load(bgPtr + off);
        Vec4f32 pf = Vec4f32::load(fgPtr + off);
        (pf + pb - pb * pf).clamp01().store(dstPtr + off);
      }
      return;

    default: break;
  }

  // General path: use float instead of double, function pointer for blend mode.
  const bool nonSeparable = isNonSeparable(mode);

  double (*blendFn)(double, double) = nullptr;
  if (!nonSeparable) {
    switch (mode) {
      case BlendMode::Darken: blendFn = blendDarken; break;
      case BlendMode::Lighten: blendFn = blendLighten; break;
      case BlendMode::Overlay: blendFn = blendOverlay; break;
      case BlendMode::ColorDodge: blendFn = blendColorDodge; break;
      case BlendMode::ColorBurn: blendFn = blendColorBurn; break;
      case BlendMode::HardLight: blendFn = blendHardLight; break;
      case BlendMode::SoftLight: blendFn = blendSoftLight; break;
      case BlendMode::Difference: blendFn = blendDifference; break;
      case BlendMode::Exclusion: blendFn = blendExclusion; break;
      default: blendFn = blendNormal; break;
    }
  }

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;

    const float bgA = bgPtr[off + 3];
    const float fgA = fgPtr[off + 3];
    const float resultA = fgA + bgA - fgA * bgA;

    if (resultA == 0.0f) {
      Vec4f32().store(dstPtr + off);
      continue;
    }

    // Unpremultiply using float reciprocal.
    const float invBgA = bgA > 0.0f ? 1.0f / bgA : 0.0f;
    const float invFgA = fgA > 0.0f ? 1.0f / fgA : 0.0f;
    const float bgR = bgPtr[off + 0] * invBgA;
    const float bgG = bgPtr[off + 1] * invBgA;
    const float bgB = bgPtr[off + 2] * invBgA;
    const float fgR = fgPtr[off + 0] * invFgA;
    const float fgG = fgPtr[off + 1] * invFgA;
    const float fgB = fgPtr[off + 2] * invFgA;

    if (nonSeparable) {
      auto blended = blendNonSeparable(mode, bgR, bgG, bgB, fgR, fgG, fgB);

      auto composite = [&](double cb, double cs, double b) -> float {
        double co = (1.0 - bgA) * fgA * cs + (1.0 - fgA) * bgA * cb + fgA * bgA * b;
        return static_cast<float>(std::clamp(co, 0.0, 1.0));
      };

      dstPtr[off + 0] = composite(bgR, fgR, blended.r);
      dstPtr[off + 1] = composite(bgG, fgG, blended.g);
      dstPtr[off + 2] = composite(bgB, fgB, blended.b);
    } else {
      auto blendChannel = [&](float cb, float cs) -> float {
        const float blended = static_cast<float>(blendFn(cb, cs));
        const float co = (1.0f - bgA) * fgA * cs + (1.0f - fgA) * bgA * cb + fgA * bgA * blended;
        return std::clamp(co, 0.0f, 1.0f);
      };

      dstPtr[off + 0] = blendChannel(bgR, fgR);
      dstPtr[off + 1] = blendChannel(bgG, fgG);
      dstPtr[off + 2] = blendChannel(bgB, fgB);
    }

    dstPtr[off + 3] = std::clamp(resultA, 0.0f, 1.0f);
  }
}

}  // namespace tiny_skia::filter
