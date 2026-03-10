#pragma once
/**
 * @file TextDecoration.h
 *
 * Defines the \ref donner::svg::TextDecoration enum for the `text-decoration` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * The parsed result of the `text-decoration` property (line type only).
 *
 * In SVG, `text-decoration` primarily specifies the decoration line type: underline, overline,
 * or line-through. The full CSS `text-decoration` shorthand (color, style, thickness) is not
 * yet supported.
 *
 * @see https://www.w3.org/TR/SVG2/text.html#TextDecorationProperties
 */
enum class TextDecoration : uint8_t {
  None,         ///< [DEFAULT] No text decoration.
  Underline,    ///< Draw a line below the text.
  Overline,     ///< Draw a line above the text.
  LineThrough,  ///< Draw a line through the middle of the text.
};

/**
 * Ostream output operator for \ref TextDecoration enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, TextDecoration value) {
  switch (value) {
    case TextDecoration::None: return os << "none";
    case TextDecoration::Underline: return os << "underline";
    case TextDecoration::Overline: return os << "overline";
    case TextDecoration::LineThrough: return os << "line-through";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
