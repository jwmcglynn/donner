#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS `mix-blend-mode` property values for compositing blend modes.
 */
enum class MixBlendMode : uint8_t {
  Normal,      //!< [DEFAULT] Source over destination (no blending effect).
  Multiply,    //!< Component-wise multiply.
  Screen,      //!< Inverse multiply.
  Overlay,     //!< Multiply or screen based on destination.
  Darken,      //!< Minimum of source and destination.
  Lighten,     //!< Maximum of source and destination.
  ColorDodge,  //!< Brighten destination toward source.
  ColorBurn,   //!< Darken destination toward source.
  HardLight,   //!< Multiply or screen based on source.
  SoftLight,   //!< Soft version of hard light.
  Difference,  //!< Absolute difference.
  Exclusion,   //!< Similar to Difference but lower contrast.
  Hue,         //!< Source hue, destination saturation and luminosity.
  Saturation,  //!< Source saturation, destination hue and luminosity.
  Color,       //!< Source hue and saturation, destination luminosity.
  Luminosity,  //!< Source luminosity, destination hue and saturation.
};

/// ostream output operator for \ref MixBlendMode.
inline std::ostream& operator<<(std::ostream& os, MixBlendMode value) {
  switch (value) {
    case MixBlendMode::Normal: return os << "normal";
    case MixBlendMode::Multiply: return os << "multiply";
    case MixBlendMode::Screen: return os << "screen";
    case MixBlendMode::Overlay: return os << "overlay";
    case MixBlendMode::Darken: return os << "darken";
    case MixBlendMode::Lighten: return os << "lighten";
    case MixBlendMode::ColorDodge: return os << "color-dodge";
    case MixBlendMode::ColorBurn: return os << "color-burn";
    case MixBlendMode::HardLight: return os << "hard-light";
    case MixBlendMode::SoftLight: return os << "soft-light";
    case MixBlendMode::Difference: return os << "difference";
    case MixBlendMode::Exclusion: return os << "exclusion";
    case MixBlendMode::Hue: return os << "hue";
    case MixBlendMode::Saturation: return os << "saturation";
    case MixBlendMode::Color: return os << "color";
    case MixBlendMode::Luminosity: return os << "luminosity";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
