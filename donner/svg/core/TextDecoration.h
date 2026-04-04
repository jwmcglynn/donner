#pragma once
/**
 * @file TextDecoration.h
 *
 * Defines the \ref donner::svg::TextDecoration bitmask for the `text-decoration` CSS property.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Bitmask for the `text-decoration` property line types.
 *
 * Values can be combined: `Underline | Overline` represents both an underline and overline.
 * The CSS `text-decoration` shorthand parses space-separated values into this bitmask.
 *
 * @see https://www.w3.org/TR/SVG2/text.html#TextDecorationProperties
 */
enum class TextDecoration : uint8_t {
  None = 0,             ///< [DEFAULT] No text decoration.
  Underline = 1 << 0,   ///< Draw a line below the text.
  Overline = 1 << 1,    ///< Draw a line above the text.
  LineThrough = 1 << 2,  ///< Draw a line through the middle of the text.
};

/// Bitwise OR for combining TextDecoration values.
constexpr TextDecoration operator|(TextDecoration a, TextDecoration b) {
  return static_cast<TextDecoration>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// Bitwise AND for testing TextDecoration flags.
constexpr TextDecoration operator&(TextDecoration a, TextDecoration b) {
  return static_cast<TextDecoration>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/// Bitwise OR assignment.
constexpr TextDecoration& operator|=(TextDecoration& a, TextDecoration b) {
  a = a | b;
  return a;
}

/// Returns true if any of the flags in \p flags are set in \p value.
constexpr bool hasFlag(TextDecoration value, TextDecoration flags) {
  return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flags)) != 0;
}

/**
 * Ostream output operator for \ref TextDecoration bitmask, outputs CSS values.
 */
inline std::ostream& operator<<(std::ostream& os, TextDecoration value) {
  if (value == TextDecoration::None) {
    return os << "none";
  }
  bool first = true;
  auto emit = [&](TextDecoration flag, const char* name) {
    if (hasFlag(value, flag)) {
      if (!first) {
        os << ' ';
      }
      os << name;
      first = false;
    }
  };
  emit(TextDecoration::Underline, "underline");
  emit(TextDecoration::Overline, "overline");
  emit(TextDecoration::LineThrough, "line-through");
  return os;
}

}  // namespace donner::svg
