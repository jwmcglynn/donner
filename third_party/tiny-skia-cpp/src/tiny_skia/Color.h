#pragma once

/// @file Color.h
/// @brief Color types (8-bit and floating-point, straight and premultiplied).

#include <array>
#include <cstdint>
#include <optional>

#include "tiny_skia/FloatingPoint.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia {

/// 8-bit alpha value.
using AlphaU8 = std::uint8_t;

constexpr AlphaU8 kAlphaU8Transparent = 0x00;
constexpr AlphaU8 kAlphaU8Opaque = 0xFF;

class Color;
class PremultipliedColor;
class PremultipliedColorU8;

/// 8-bit RGBA color (straight alpha).
class ColorU8 {
 public:
  constexpr ColorU8() = default;
  constexpr ColorU8(AlphaU8 red, AlphaU8 green, AlphaU8 blue, AlphaU8 alpha)
      : data_{red, green, blue, alpha} {}

  static constexpr ColorU8 fromRgba(AlphaU8 red, AlphaU8 green, AlphaU8 blue, AlphaU8 alpha) {
    return ColorU8(red, green, blue, alpha);
  }

  constexpr AlphaU8 red() const { return data_[0]; }
  constexpr AlphaU8 green() const { return data_[1]; }
  constexpr AlphaU8 blue() const { return data_[2]; }
  constexpr AlphaU8 alpha() const { return data_[3]; }

  [[nodiscard]] bool isOpaque() const { return alpha() == kAlphaU8Opaque; }

  /// Converts to premultiplied alpha.
  [[nodiscard]] PremultipliedColorU8 premultiply() const;

  bool operator==(const ColorU8&) const = default;

 private:
  std::array<AlphaU8, 4> data_{};
};

/// 8-bit RGBA color (premultiplied alpha). Internal pixel format.
class PremultipliedColorU8 {
 public:
  static const PremultipliedColorU8 transparent;
  constexpr PremultipliedColorU8() = default;

  /// Creates from components. Returns nullopt if any channel > alpha.
  static std::optional<PremultipliedColorU8> fromRgba(AlphaU8 red, AlphaU8 green, AlphaU8 blue,
                                                      AlphaU8 alpha);
  /// Creates from components without validation.
  static constexpr PremultipliedColorU8 fromRgbaUnchecked(AlphaU8 red, AlphaU8 green, AlphaU8 blue,
                                                          AlphaU8 alpha) {
    return PremultipliedColorU8(red, green, blue, alpha);
  }

  constexpr PremultipliedColorU8(AlphaU8 red, AlphaU8 green, AlphaU8 blue, AlphaU8 alpha)
      : data_{red, green, blue, alpha} {}

  constexpr AlphaU8 red() const { return data_[0]; }
  constexpr AlphaU8 green() const { return data_[1]; }
  constexpr AlphaU8 blue() const { return data_[2]; }
  constexpr AlphaU8 alpha() const { return data_[3]; }

  [[nodiscard]] bool isOpaque() const { return alpha() == kAlphaU8Opaque; }

  /// Converts to straight alpha.
  [[nodiscard]] ColorU8 demultiply() const;

  bool operator==(const PremultipliedColorU8&) const = default;

 private:
  std::array<AlphaU8, 4> data_{};
};

/// Floating-point RGBA color [0,1] (straight alpha).
class Color {
 public:
  Color() = default;
  static const Color transparent;
  static const Color black;
  static const Color white;

  constexpr Color(NormalizedF32 red, NormalizedF32 green, NormalizedF32 blue, NormalizedF32 alpha)
      : red_(red), green_(green), blue_(blue), alpha_(alpha) {}

  /// Creates without validation (values must be in [0,1]).
  static Color fromRgbaUnchecked(float red, float green, float blue, float alpha);
  /// Creates with validation. Returns nullopt if any component is outside [0,1].
  static std::optional<Color> fromRgba(float red, float green, float blue, float alpha);
  /// Creates from 8-bit components.
  static Color fromRgba8(AlphaU8 red, AlphaU8 green, AlphaU8 blue, AlphaU8 alpha);

  float red() const { return red_.get(); }
  float green() const { return green_.get(); }
  float blue() const { return blue_.get(); }
  float alpha() const { return alpha_.get(); }

  void setRed(float value) { red_ = NormalizedF32::newClamped(value); }
  void setGreen(float value) { green_ = NormalizedF32::newClamped(value); }
  void setBlue(float value) { blue_ = NormalizedF32::newClamped(value); }
  void setAlpha(float value) { alpha_ = NormalizedF32::newClamped(value); }

  /// Multiplies alpha by opacity (clamped to [0,1]).
  void applyOpacity(float opacity) {
    alpha_ = NormalizedF32::newClamped(alpha_.get() * bound(0.0f, opacity, 1.0f));
  }

  [[nodiscard]] bool isOpaque() const { return alpha_ == NormalizedF32::one(); }

  /// Converts to premultiplied alpha.
  [[nodiscard]] PremultipliedColor premultiply() const;
  /// Converts to 8-bit color.
  [[nodiscard]] ColorU8 toColorU8() const;
  bool operator==(const Color&) const = default;

 private:
  NormalizedF32 red_ = NormalizedF32::zero();
  NormalizedF32 green_ = NormalizedF32::zero();
  NormalizedF32 blue_ = NormalizedF32::zero();
  NormalizedF32 alpha_ = NormalizedF32::zero();
};

/// Floating-point RGBA color [0,1] (premultiplied alpha).
class PremultipliedColor {
 public:
  constexpr PremultipliedColor() = default;
  constexpr PremultipliedColor(NormalizedF32 red, NormalizedF32 green, NormalizedF32 blue,
                               NormalizedF32 alpha)
      : red_(red), green_(green), blue_(blue), alpha_(alpha) {}

  float red() const { return red_.get(); }
  float green() const { return green_.get(); }
  float blue() const { return blue_.get(); }
  float alpha() const { return alpha_.get(); }

  /// Converts to straight alpha.
  [[nodiscard]] Color demultiply() const;
  /// Converts to 8-bit premultiplied color.
  [[nodiscard]] PremultipliedColorU8 toColorU8() const;

  bool operator==(const PremultipliedColor&) const = default;

 private:
  friend class Color;
  NormalizedF32 red_ = NormalizedF32::zero();
  NormalizedF32 green_ = NormalizedF32::zero();
  NormalizedF32 blue_ = NormalizedF32::zero();
  NormalizedF32 alpha_ = NormalizedF32::zero();
};

/// @internal
AlphaU8 premultiplyU8(AlphaU8 color, AlphaU8 alpha);

/// Colorspace for gamma-correct blending.
enum class ColorSpace {
  Linear,       ///< Linear RGB (no gamma).
  Gamma2,       ///< Power-of-2 gamma approximation.
  SimpleSRGB,   ///< Simplified sRGB transfer function.
  FullSRGBGamma,///< Full sRGB gamma curve.
};

/// @internal
NormalizedF32 expandChannel(ColorSpace colorSpace, NormalizedF32 x);
/// @internal
Color expandColor(ColorSpace colorSpace, Color color);
/// @internal
NormalizedF32 compressChannel(ColorSpace colorSpace, NormalizedF32 x);

/// @internal
std::optional<pipeline::Stage> expandStage(ColorSpace colorSpace);
/// @internal
std::optional<pipeline::Stage> expandDestStage(ColorSpace colorSpace);
/// @internal
std::optional<pipeline::Stage> compressStage(ColorSpace colorSpace);

}  // namespace tiny_skia
