#include "tiny_skia/Color.h"

#include <array>
#include <cmath>

namespace tiny_skia {

PremultipliedColorU8 ColorU8::premultiply() const {
  const auto a = alpha();
  if (a == kAlphaU8Opaque) {
    return PremultipliedColorU8::fromRgbaUnchecked(red(), green(), blue(), a);
  }
  return PremultipliedColorU8::fromRgbaUnchecked(premultiplyU8(red(), a), premultiplyU8(green(), a),
                                                 premultiplyU8(blue(), a), a);
}

const PremultipliedColorU8 PremultipliedColorU8::transparent =
    PremultipliedColorU8::fromRgbaUnchecked(0, 0, 0, 0);

std::optional<PremultipliedColorU8> PremultipliedColorU8::fromRgba(AlphaU8 red, AlphaU8 green,
                                                                   AlphaU8 blue, AlphaU8 alpha) {
  if (red <= alpha && green <= alpha && blue <= alpha) {
    return fromRgbaUnchecked(red, green, blue, alpha);
  }
  return std::nullopt;
}

ColorU8 PremultipliedColorU8::demultiply() const {
  const auto a = alpha();
  if (a == kAlphaU8Opaque) {
    return ColorU8(red(), green(), blue(), a);
  }
  const auto alphaF = static_cast<float>(a) / 255.0f;
  if (alphaF == 0.0f) {
    return ColorU8(0, 0, 0, 0);
  }
  return ColorU8(static_cast<AlphaU8>(static_cast<float>(red()) / alphaF + 0.5f),
                 static_cast<AlphaU8>(static_cast<float>(green()) / alphaF + 0.5f),
                 static_cast<AlphaU8>(static_cast<float>(blue()) / alphaF + 0.5f), a);
}

const Color Color::transparent = Color::fromRgbaUnchecked(0.0f, 0.0f, 0.0f, 0.0f);
const Color Color::black = Color::fromRgbaUnchecked(0.0f, 0.0f, 0.0f, 1.0f);
const Color Color::white = Color::fromRgbaUnchecked(1.0f, 1.0f, 1.0f, 1.0f);

Color Color::fromRgbaUnchecked(float red, float green, float blue, float alpha) {
  return Color(NormalizedF32::newUnchecked(red), NormalizedF32::newUnchecked(green),
               NormalizedF32::newUnchecked(blue), NormalizedF32::newUnchecked(alpha));
}

std::optional<Color> Color::fromRgba(float red, float green, float blue, float alpha) {
  const auto r = NormalizedF32::newFloat(red);
  const auto g = NormalizedF32::newFloat(green);
  const auto b = NormalizedF32::newFloat(blue);
  const auto a = NormalizedF32::newFloat(alpha);
  if (r.has_value() && g.has_value() && b.has_value() && a.has_value()) {
    return Color(r.value(), g.value(), b.value(), a.value());
  }
  return std::nullopt;
}

Color Color::fromRgba8(AlphaU8 red, AlphaU8 green, AlphaU8 blue, AlphaU8 alpha) {
  return Color::fromRgbaUnchecked(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
}

PremultipliedColor Color::premultiply() const {
  if (isOpaque()) {
    return PremultipliedColor(red_, green_, blue_, alpha_);
  }
  return PremultipliedColor(NormalizedF32::newClamped(red_.get() * alpha_.get()),
                            NormalizedF32::newClamped(green_.get() * alpha_.get()),
                            NormalizedF32::newClamped(blue_.get() * alpha_.get()), alpha_);
}

namespace {

std::array<std::uint8_t, 4> colorF32ToU8(NormalizedF32 red, NormalizedF32 green, NormalizedF32 blue,
                                         NormalizedF32 alpha) {
  return {
      static_cast<std::uint8_t>(red.get() * 255.0f + 0.5f),
      static_cast<std::uint8_t>(green.get() * 255.0f + 0.5f),
      static_cast<std::uint8_t>(blue.get() * 255.0f + 0.5f),
      static_cast<std::uint8_t>(alpha.get() * 255.0f + 0.5f),
  };
}

}  // namespace

ColorU8 Color::toColorU8() const {
  const auto c = colorF32ToU8(red_, green_, blue_, alpha_);
  return ColorU8::fromRgba(c[0], c[1], c[2], c[3]);
}

Color PremultipliedColor::demultiply() const {
  const auto a = alpha();
  if (a == 0.0f) {
    return Color::transparent;
  }
  return Color(NormalizedF32::newClamped(red_.get() / a),
               NormalizedF32::newClamped(green_.get() / a),
               NormalizedF32::newClamped(blue_.get() / a), NormalizedF32::newClamped(a));
}

PremultipliedColorU8 PremultipliedColor::toColorU8() const {
  const auto c = colorF32ToU8(red_, green_, blue_, alpha_);
  return PremultipliedColorU8::fromRgbaUnchecked(c[0], c[1], c[2], c[3]);
}

AlphaU8 premultiplyU8(AlphaU8 color, AlphaU8 alpha) {
  const auto prod = static_cast<std::uint32_t>(color) * static_cast<std::uint32_t>(alpha) + 128;
  return static_cast<AlphaU8>((prod + (prod >> 8)) >> 8);
}

NormalizedF32 expandChannel(ColorSpace colorSpace, NormalizedF32 x) {
  switch (colorSpace) {
    case ColorSpace::Linear:
      return x;
    case ColorSpace::Gamma2:
      return NormalizedF32::newUnchecked(x.get() * x.get());
    case ColorSpace::SimpleSRGB:
      return NormalizedF32::newClamped(approxPowf(x.get(), 2.2f));
    case ColorSpace::FullSRGBGamma: {
      const auto xValue = x.get();
      if (xValue <= 0.04045f) {
        return NormalizedF32::newUnchecked(xValue / 12.92f);
      }
      return NormalizedF32::newClamped(approxPowf((xValue + 0.055f) / 1.055f, 2.4f));
    }
  }
  return x;  // unreachable; satisfies -Wreturn-type
}

Color expandColor(ColorSpace colorSpace, Color color) {
  color.setRed(expandChannel(colorSpace, NormalizedF32::newUnchecked(color.red())).get());
  color.setGreen(expandChannel(colorSpace, NormalizedF32::newUnchecked(color.green())).get());
  color.setBlue(expandChannel(colorSpace, NormalizedF32::newUnchecked(color.blue())).get());
  return color;
}

NormalizedF32 compressChannel(ColorSpace colorSpace, NormalizedF32 x) {
  switch (colorSpace) {
    case ColorSpace::Linear:
      return x;
    case ColorSpace::Gamma2:
      return NormalizedF32::newUnchecked(std::sqrt(x.get()));
    case ColorSpace::SimpleSRGB:
      return NormalizedF32::newClamped(approxPowf(x.get(), 0.45454545f));
    case ColorSpace::FullSRGBGamma: {
      const auto xValue = x.get();
      if (xValue <= 0.0031308f) {
        return NormalizedF32::newUnchecked(xValue * 12.92f);
      }
      return NormalizedF32::newClamped(approxPowf(xValue, 1.0f / 2.4f) * 1.055f - 0.055f);
    }
  }
  return x;  // unreachable; satisfies -Wreturn-type
}

std::optional<pipeline::Stage> expandStage(ColorSpace colorSpace) {
  switch (colorSpace) {
    case ColorSpace::Linear:
      return std::nullopt;
    case ColorSpace::Gamma2:
      return pipeline::Stage::GammaExpand2;
    case ColorSpace::SimpleSRGB:
      return pipeline::Stage::GammaExpand22;
    case ColorSpace::FullSRGBGamma:
      return pipeline::Stage::GammaExpandSrgb;
  }
  return std::nullopt;  // unreachable; satisfies -Wreturn-type
}

std::optional<pipeline::Stage> expandDestStage(ColorSpace colorSpace) {
  switch (colorSpace) {
    case ColorSpace::Linear:
      return std::nullopt;
    case ColorSpace::Gamma2:
      return pipeline::Stage::GammaExpandDestination2;
    case ColorSpace::SimpleSRGB:
      return pipeline::Stage::GammaExpandDestination22;
    case ColorSpace::FullSRGBGamma:
      return pipeline::Stage::GammaExpandDestinationSrgb;
  }
  return std::nullopt;  // unreachable; satisfies -Wreturn-type
}

std::optional<pipeline::Stage> compressStage(ColorSpace colorSpace) {
  switch (colorSpace) {
    case ColorSpace::Linear:
      return std::nullopt;
    case ColorSpace::Gamma2:
      return pipeline::Stage::GammaCompress2;
    case ColorSpace::SimpleSRGB:
      return pipeline::Stage::GammaCompress22;
    case ColorSpace::FullSRGBGamma:
      return pipeline::Stage::GammaCompressSrgb;
  }
  return std::nullopt;  // unreachable; satisfies -Wreturn-type
}

}  // namespace tiny_skia
