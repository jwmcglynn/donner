#pragma once
/**
 * @file FontStyle.h
 *
 * Defines the \ref donner::svg::FontStyle enum for the `font-style` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `font-style` property.
 *
 * @see https://www.w3.org/TR/css-fonts-4/#font-style-prop
 */
enum class FontStyle : uint8_t {
  Normal,   ///< [DEFAULT] Normal (upright) style.
  Italic,   ///< Italic style, uses a specifically designed italic face if available.
  Oblique,  ///< Oblique style, a slanted version of the normal face.
};

/**
 * Ostream output operator for \ref FontStyle enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, FontStyle value) {
  switch (value) {
    case FontStyle::Normal: return os << "normal";
    case FontStyle::Italic: return os << "italic";
    case FontStyle::Oblique: return os << "oblique";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
