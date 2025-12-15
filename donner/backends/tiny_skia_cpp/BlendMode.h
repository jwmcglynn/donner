#pragma once
/// @file

#include <array>
#include <cstdint>
#include <ostream>

#include "donner/backends/tiny_skia_cpp/Color.h"

namespace donner::backends::tiny_skia_cpp {

/** Supported Porter-Duff and Photoshop-style blend modes. */
enum class BlendMode : uint8_t {
  kClear,
  kSource,
  kDestination,
  kSourceOver,
  kDestinationOver,
  kSourceIn,
  kDestinationIn,
  kSourceOut,
  kDestinationOut,
  kSourceAtop,
  kDestinationAtop,
  kXor,
  kPlus,
  kModulate,
  kScreen,
  kOverlay,
  kDarken,
  kLighten,
  kColorDodge,
  kColorBurn,
  kHardLight,
  kSoftLight,
  kDifference,
  kExclusion,
  kMultiply,
  kHue,
  kSaturation,
  kColor,
  kLuminosity,
};

std::ostream& operator<<(std::ostream& os, BlendMode mode);

/** Premultiplied RGBA color stored as normalized floats. */
struct PremultipliedColorF {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 0.0;
};

/** Premultiplies an 8-bit RGBA color into normalized floats. */
PremultipliedColorF Premultiply(const Color& color);

/** Converts a normalized premultiplied color to 8-bit channels with rounding. */
Color ToColor(const PremultipliedColorF& color);

/** Blends premultiplied source and destination colors using the requested mode. */
PremultipliedColorF Blend(const PremultipliedColorF& source, const PremultipliedColorF& dest,
                          BlendMode mode);

}  // namespace donner::backends::tiny_skia_cpp
