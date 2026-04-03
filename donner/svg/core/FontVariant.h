#pragma once
/**
 * @file FontVariant.h
 *
 * Defines the \ref donner::svg::FontVariant enum for the `font-variant` CSS property.
 *
 * This covers the SVG 1.1 shorthand values (normal / small-caps). Full CSS Fonts Level 4
 * sub-properties (font-variant-numeric, etc.) are not yet supported.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `font-variant` shorthand property (SVG 1.1 subset).
 *
 * @see https://www.w3.org/TR/CSS21/fonts.html#propdef-font-variant
 */
enum class FontVariant : uint8_t {
  Normal,    ///< [DEFAULT] Normal variant.
  SmallCaps, ///< Small-caps variant; lowercase letters rendered as smaller uppercase glyphs.
};

/**
 * Ostream output operator for \ref FontVariant enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, FontVariant value) {
  switch (value) {
    case FontVariant::Normal: return os << "normal";
    case FontVariant::SmallCaps: return os << "small-caps";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
