#include "donner/backends/tiny_skia_cpp/BlendMode.h"

#include <algorithm>
#include <cmath>

namespace donner::backends::tiny_skia_cpp {
namespace {

constexpr double kEpsilon = 1e-9;

double clamp01(double value) {
  return std::clamp(value, 0.0, 1.0);
}

PremultipliedColorF unpremultiply(const PremultipliedColorF& color) {
  if (color.a < kEpsilon) {
    return {};
  }

  const double invAlpha = 1.0 / color.a;
  return {color.r * invAlpha, color.g * invAlpha, color.b * invAlpha, color.a};
}

PremultipliedColorF blendSeparable(const PremultipliedColorF& source,
                                   const PremultipliedColorF& dest,
                                   const std::array<double, 3>& blended) {
  const double sa = source.a;
  const double da = dest.a;
  const double outAlpha = sa + da - sa * da;

  PremultipliedColorF result{};
  for (int i = 0; i < 3; ++i) {
    const double s = i == 0 ? source.r : (i == 1 ? source.g : source.b);
    const double d = i == 0 ? dest.r : (i == 1 ? dest.g : dest.b);
    result.r = i == 0 ? (blended[i] * sa * da + s * (1.0 - da) + d * (1.0 - sa)) : result.r;
    result.g = i == 1 ? (blended[i] * sa * da + s * (1.0 - da) + d * (1.0 - sa)) : result.g;
    result.b = i == 2 ? (blended[i] * sa * da + s * (1.0 - da) + d * (1.0 - sa)) : result.b;
  }

  result.a = outAlpha;
  return result;
}

double luminance(double r, double g, double b) {
  return 0.3 * r + 0.59 * g + 0.11 * b;
}

std::array<double, 3> setLuminance(const std::array<double, 3>& color, double targetLum) {
  const double currentLum = luminance(color[0], color[1], color[2]);
  const double delta = targetLum - currentLum;
  std::array<double, 3> adjusted{color[0] + delta, color[1] + delta, color[2] + delta};

  double minChannel = std::min({adjusted[0], adjusted[1], adjusted[2]});
  double maxChannel = std::max({adjusted[0], adjusted[1], adjusted[2]});
  if (minChannel < 0.0) {
    adjusted[0] = targetLum + (adjusted[0] - targetLum) * targetLum / (targetLum - minChannel);
    adjusted[1] = targetLum + (adjusted[1] - targetLum) * targetLum / (targetLum - minChannel);
    adjusted[2] = targetLum + (adjusted[2] - targetLum) * targetLum / (targetLum - minChannel);
  }
  if (maxChannel > 1.0) {
    const double invMax = (1.0 - targetLum) / (maxChannel - targetLum);
    adjusted[0] = targetLum + (adjusted[0] - targetLum) * invMax;
    adjusted[1] = targetLum + (adjusted[1] - targetLum) * invMax;
    adjusted[2] = targetLum + (adjusted[2] - targetLum) * invMax;
  }

  return adjusted;
}

std::array<double, 3> setSaturation(const std::array<double, 3>& color, double saturation) {
  double maxChannel = std::max({color[0], color[1], color[2]});
  double minChannel = std::min({color[0], color[1], color[2]});
  if (maxChannel - minChannel < kEpsilon) {
    return {0.0, 0.0, 0.0};
  }

  std::array<double, 3> result = color;
  const int maxIndex = maxChannel == color[0] ? 0 : (maxChannel == color[1] ? 1 : 2);
  const int minIndex = minChannel == color[0] ? 0 : (minChannel == color[1] ? 1 : 2);
  const int midIndex = 3 - maxIndex - minIndex;

  result[maxIndex] = saturation;
  result[midIndex] = saturation * (color[midIndex] - minChannel) / (maxChannel - minChannel);
  result[minIndex] = 0.0;
  return result;
}

std::array<double, 3> blendFunction(BlendMode mode, const PremultipliedColorF& source,
                                    const PremultipliedColorF& dest) {
  const PremultipliedColorF s = unpremultiply(source);
  const PremultipliedColorF d = unpremultiply(dest);
  switch (mode) {
    case BlendMode::kMultiply: return {s.r * d.r, s.g * d.g, s.b * d.b};
    case BlendMode::kScreen:
      return {s.r + d.r - s.r * d.r, s.g + d.g - s.g * d.g, s.b + d.b - s.b * d.b};
    case BlendMode::kOverlay:
      return {d.r <= 0.5 ? 2.0 * s.r * d.r : 1.0 - 2.0 * (1.0 - s.r) * (1.0 - d.r),
              d.g <= 0.5 ? 2.0 * s.g * d.g : 1.0 - 2.0 * (1.0 - s.g) * (1.0 - d.g),
              d.b <= 0.5 ? 2.0 * s.b * d.b : 1.0 - 2.0 * (1.0 - s.b) * (1.0 - d.b)};
    case BlendMode::kDarken: return {std::min(s.r, d.r), std::min(s.g, d.g), std::min(s.b, d.b)};
    case BlendMode::kLighten: return {std::max(s.r, d.r), std::max(s.g, d.g), std::max(s.b, d.b)};
    case BlendMode::kColorDodge:
      return {s.r >= 1.0 ? 1.0 : std::min(1.0, d.r / std::max(1.0 - s.r, kEpsilon)),
              s.g >= 1.0 ? 1.0 : std::min(1.0, d.g / std::max(1.0 - s.g, kEpsilon)),
              s.b >= 1.0 ? 1.0 : std::min(1.0, d.b / std::max(1.0 - s.b, kEpsilon))};
    case BlendMode::kColorBurn:
      return {s.r <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - d.r) / s.r),
              s.g <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - d.g) / s.g),
              s.b <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - d.b) / s.b)};
    case BlendMode::kHardLight:
      return {s.r <= 0.5 ? 2.0 * s.r * d.r : 1.0 - 2.0 * (1.0 - s.r) * (1.0 - d.r),
              s.g <= 0.5 ? 2.0 * s.g * d.g : 1.0 - 2.0 * (1.0 - s.g) * (1.0 - d.g),
              s.b <= 0.5 ? 2.0 * s.b * d.b : 1.0 - 2.0 * (1.0 - s.b) * (1.0 - d.b)};
    case BlendMode::kSoftLight: {
      auto soft = [](double sValue, double dValue) {
        if (sValue <= 0.5) {
          return dValue - (1.0 - 2.0 * sValue) * dValue * (1.0 - dValue);
        }
        const double g =
            dValue <= 0.25 ? ((16.0 * dValue - 12.0) * dValue + 4.0) * dValue : std::sqrt(dValue);
        return dValue + (2.0 * sValue - 1.0) * (g - dValue);
      };
      return {soft(s.r, d.r), soft(s.g, d.g), soft(s.b, d.b)};
    }
    case BlendMode::kDifference:
      return {std::abs(d.r - s.r), std::abs(d.g - s.g), std::abs(d.b - s.b)};
    case BlendMode::kExclusion:
      return {d.r + s.r - 2.0 * d.r * s.r, d.g + s.g - 2.0 * d.g * s.g,
              d.b + s.b - 2.0 * d.b * s.b};
    case BlendMode::kHue: {
      const std::array<double, 3> result = setLuminance(
          setSaturation({s.r, s.g, s.b}, std::max({d.r, d.g, d.b}) - std::min({d.r, d.g, d.b})),
          luminance(d.r, d.g, d.b));
      return {result[0], result[1], result[2]};
    }
    case BlendMode::kSaturation: {
      const std::array<double, 3> result = setLuminance(
          setSaturation({d.r, d.g, d.b}, std::max({s.r, s.g, s.b}) - std::min({s.r, s.g, s.b})),
          luminance(d.r, d.g, d.b));
      return {result[0], result[1], result[2]};
    }
    case BlendMode::kColor: {
      const std::array<double, 3> result = setLuminance({s.r, s.g, s.b}, luminance(d.r, d.g, d.b));
      return {result[0], result[1], result[2]};
    }
    case BlendMode::kLuminosity: {
      const std::array<double, 3> result = setLuminance({d.r, d.g, d.b}, luminance(s.r, s.g, s.b));
      return {result[0], result[1], result[2]};
    }
    default: return {};
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, BlendMode mode) {
  switch (mode) {
    case BlendMode::kClear: return os << "BlendMode::kClear";
    case BlendMode::kSource: return os << "BlendMode::kSource";
    case BlendMode::kDestination: return os << "BlendMode::kDestination";
    case BlendMode::kSourceOver: return os << "BlendMode::kSourceOver";
    case BlendMode::kDestinationOver: return os << "BlendMode::kDestinationOver";
    case BlendMode::kSourceIn: return os << "BlendMode::kSourceIn";
    case BlendMode::kDestinationIn: return os << "BlendMode::kDestinationIn";
    case BlendMode::kSourceOut: return os << "BlendMode::kSourceOut";
    case BlendMode::kDestinationOut: return os << "BlendMode::kDestinationOut";
    case BlendMode::kSourceAtop: return os << "BlendMode::kSourceAtop";
    case BlendMode::kDestinationAtop: return os << "BlendMode::kDestinationAtop";
    case BlendMode::kXor: return os << "BlendMode::kXor";
    case BlendMode::kPlus: return os << "BlendMode::kPlus";
    case BlendMode::kModulate: return os << "BlendMode::kModulate";
    case BlendMode::kScreen: return os << "BlendMode::kScreen";
    case BlendMode::kOverlay: return os << "BlendMode::kOverlay";
    case BlendMode::kDarken: return os << "BlendMode::kDarken";
    case BlendMode::kLighten: return os << "BlendMode::kLighten";
    case BlendMode::kColorDodge: return os << "BlendMode::kColorDodge";
    case BlendMode::kColorBurn: return os << "BlendMode::kColorBurn";
    case BlendMode::kHardLight: return os << "BlendMode::kHardLight";
    case BlendMode::kSoftLight: return os << "BlendMode::kSoftLight";
    case BlendMode::kDifference: return os << "BlendMode::kDifference";
    case BlendMode::kExclusion: return os << "BlendMode::kExclusion";
    case BlendMode::kMultiply: return os << "BlendMode::kMultiply";
    case BlendMode::kHue: return os << "BlendMode::kHue";
    case BlendMode::kSaturation: return os << "BlendMode::kSaturation";
    case BlendMode::kColor: return os << "BlendMode::kColor";
    case BlendMode::kLuminosity: return os << "BlendMode::kLuminosity";
  }
  return os;
}

PremultipliedColorF Premultiply(const Color& color) {
  const double alpha = static_cast<double>(color.a) / 255.0;
  const double premultR = static_cast<double>((color.r * color.a + 127) / 255) / 255.0;
  const double premultG = static_cast<double>((color.g * color.a + 127) / 255) / 255.0;
  const double premultB = static_cast<double>((color.b * color.a + 127) / 255) / 255.0;
  return {premultR, premultG, premultB, alpha};
}

Color ToColor(const PremultipliedColorF& color) {
  const uint8_t r = static_cast<uint8_t>(std::round(clamp01(color.r) * 255.0f));
  const uint8_t g = static_cast<uint8_t>(std::round(clamp01(color.g) * 255.0f));
  const uint8_t b = static_cast<uint8_t>(std::round(clamp01(color.b) * 255.0f));
  const uint8_t a = static_cast<uint8_t>(std::round(clamp01(color.a) * 255.0f));
  return Color(r, g, b, a);
}

PremultipliedColorF Blend(const PremultipliedColorF& source, const PremultipliedColorF& dest,
                          BlendMode mode) {
  switch (mode) {
    case BlendMode::kClear: return {};
    case BlendMode::kSource: return source;
    case BlendMode::kDestination: return dest;
    case BlendMode::kSourceOver: {
      const double invSa = 1.0 - source.a;
      return {source.r + dest.r * invSa, source.g + dest.g * invSa, source.b + dest.b * invSa,
              source.a + dest.a * invSa};
    }
    case BlendMode::kDestinationOver: {
      const double invDa = 1.0 - dest.a;
      return {dest.r + source.r * invDa, dest.g + source.g * invDa, dest.b + source.b * invDa,
              dest.a + source.a * invDa};
    }
    case BlendMode::kSourceIn:
      return {source.r * dest.a, source.g * dest.a, source.b * dest.a, source.a * dest.a};
    case BlendMode::kDestinationIn:
      return {dest.r * source.a, dest.g * source.a, dest.b * source.a, dest.a * source.a};
    case BlendMode::kSourceOut: {
      const double invDa = 1.0 - dest.a;
      return {source.r * invDa, source.g * invDa, source.b * invDa, source.a * invDa};
    }
    case BlendMode::kDestinationOut: {
      const double invSa = 1.0 - source.a;
      return {dest.r * invSa, dest.g * invSa, dest.b * invSa, dest.a * invSa};
    }
    case BlendMode::kSourceAtop: {
      const double invSa = 1.0 - source.a;
      return {source.r * dest.a + dest.r * invSa, source.g * dest.a + dest.g * invSa,
              source.b * dest.a + dest.b * invSa, dest.a};
    }
    case BlendMode::kDestinationAtop: {
      const double invDa = 1.0 - dest.a;
      return {dest.r * source.a + source.r * invDa, dest.g * source.a + source.g * invDa,
              dest.b * source.a + source.b * invDa, source.a};
    }
    case BlendMode::kXor: {
      const double invSa = 1.0 - source.a;
      const double invDa = 1.0 - dest.a;
      return {source.r * invDa + dest.r * invSa, source.g * invDa + dest.g * invSa,
              source.b * invDa + dest.b * invSa, source.a * invDa + dest.a * invSa};
    }
    case BlendMode::kPlus:
      return {clamp01(source.r + dest.r), clamp01(source.g + dest.g), clamp01(source.b + dest.b),
              clamp01(source.a + dest.a)};
    case BlendMode::kModulate:
      return {source.r * dest.r, source.g * dest.g, source.b * dest.b, source.a * dest.a};
    case BlendMode::kMultiply:
    case BlendMode::kScreen:
    case BlendMode::kOverlay:
    case BlendMode::kDarken:
    case BlendMode::kLighten:
    case BlendMode::kColorDodge:
    case BlendMode::kColorBurn:
    case BlendMode::kHardLight:
    case BlendMode::kSoftLight:
    case BlendMode::kDifference:
    case BlendMode::kExclusion:
    case BlendMode::kHue:
    case BlendMode::kSaturation:
    case BlendMode::kColor:
    case BlendMode::kLuminosity: {
      const std::array<double, 3> blended = blendFunction(mode, source, dest);
      return blendSeparable(source, dest, blended);
    }
  }
  return source;
}

}  // namespace donner::backends::tiny_skia_cpp
