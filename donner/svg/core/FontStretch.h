#pragma once
/**
 * @file FontStretch.h
 *
 * Defines the \ref donner::svg::FontStretch enum for the `font-stretch` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `font-stretch` property.
 *
 * Values are ordered from narrowest to widest to allow relative keyword resolution.
 *
 * @see https://www.w3.org/TR/css-fonts-4/#font-stretch-prop
 */
enum class FontStretch : uint8_t {
  UltraCondensed = 1,  ///< Ultra-condensed width (50%).
  ExtraCondensed = 2,  ///< Extra-condensed width (62.5%).
  Condensed = 3,       ///< Condensed width (75%).
  SemiCondensed = 4,   ///< Semi-condensed width (87.5%).
  Normal = 5,          ///< [DEFAULT] Normal width (100%).
  SemiExpanded = 6,    ///< Semi-expanded width (112.5%).
  Expanded = 7,        ///< Expanded width (125%).
  ExtraExpanded = 8,   ///< Extra-expanded width (150%).
  UltraExpanded = 9,   ///< Ultra-expanded width (200%).
};

/**
 * Ostream output operator for \ref FontStretch enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, FontStretch value) {
  switch (value) {
    case FontStretch::UltraCondensed: return os << "ultra-condensed";
    case FontStretch::ExtraCondensed: return os << "extra-condensed";
    case FontStretch::Condensed: return os << "condensed";
    case FontStretch::SemiCondensed: return os << "semi-condensed";
    case FontStretch::Normal: return os << "normal";
    case FontStretch::SemiExpanded: return os << "semi-expanded";
    case FontStretch::Expanded: return os << "expanded";
    case FontStretch::ExtraExpanded: return os << "extra-expanded";
    case FontStretch::UltraExpanded: return os << "ultra-expanded";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
