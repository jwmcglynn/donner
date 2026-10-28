#pragma once
/**
 * @file TextDecoration.h
 *
 * Defines the \ref donner::svg::TextDecoration bitmask for the `text-decoration` CSS property.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>

#include "donner/base/Length.h"
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
  None = 0,              ///< [DEFAULT] No text decoration.
  Underline = 1 << 0,    ///< Draw a line below the text.
  Overline = 1 << 1,     ///< Draw a line above the text.
  LineThrough = 1 << 2,  ///< Draw a line through the middle of the text.
};

/// CSS thickness of decoration lines originating on an element.
struct TextDecorationThickness {
  /// Select automatic font metrics, preferred underline metrics, or an explicit dimension.
  enum class Kind { Auto, FromFont, Length };
  Kind kind = Kind::Auto;  ///< Thickness interpretation.
  Lengthd value;           ///< Length or percentage when kind is Length.
  bool operator==(const TextDecorationThickness&) const = default;
};

/// Print a CSS decoration thickness value.
inline std::ostream& operator<<(std::ostream& os, const TextDecorationThickness& thickness) {
  switch (thickness.kind) {
    case TextDecorationThickness::Kind::Auto: return os << "auto";
    case TextDecorationThickness::Kind::FromFont: return os << "from-font";
    case TextDecorationThickness::Kind::Length: return os << thickness.value;
  }
  UTILS_UNREACHABLE();
}

/// Return whether a resolved width can enter renderer geometry.
inline bool IsValidDecorationWidth(double value) {
  return value > 0 && std::isfinite(value) && value <= std::numeric_limits<float>::max() / 4.0;
}

/// Resolve a decoration width using the originating declaration and font metrics.
/// @param thickness Origin's computed thickness (explicit dimensions are in local pixels).
/// @param automatic Automatic width for this line type.
/// @param underline Preferred underline width for from-font.
/// @param deviceScale Device pixels per local thickness unit.
inline double ResolveDecorationThickness(const TextDecorationThickness& thickness, double automatic,
                                         double underline, double deviceScale) {
  double result = automatic;
  if (thickness.kind == TextDecorationThickness::Kind::FromFont && underline > 0 &&
      std::isfinite(underline)) {
    result = underline;
  } else if (thickness.kind == TextDecorationThickness::Kind::Length) {
    if (!(deviceScale > 0) || !std::isfinite(deviceScale) ||
        !std::isfinite(thickness.value.value) || thickness.value.value < 0) {
      return 0;
    }
    result = std::max(thickness.value.value, 1.0 / deviceScale);
  }
  return IsValidDecorationWidth(result) ? result : 0;
}

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
