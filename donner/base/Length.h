#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/MathUtils.h"
#include "donner/base/RelativeLengthMetrics.h"

namespace donner {

/**
 * The unit identifier for a length, corresponding to CSS unit identifiers.
 * See https://www.w3.org/TR/css-values-3/#lengths for definitions.
 */
enum class LengthUnit {
  None,     ///< Unitless.
  Percent,  ///< Percentage, using the '\%' symbol.
  /**
   * @addtogroup _ABSOLUTE
   * Absolute lengths, https://www.w3.org/TR/css-values-3/#absolute-lengths.
   *
   * @{
   */
  Cm,  ///< Centimeters, 1cm = 96px/2.54.
  Mm,  ///< Millimeters, 1mm = 1/10th of 1cm.
  Q,   ///< Quarter-millimeters, 1Q = 1/40th of 1cm.
  In,  ///< Inches, 1in = 2.54cm = 96px.
  Pc,  ///< Picas, 1pc = 1/6th of 1in
  Pt,  ///< Points, 1pt = 1/72nd of 1in.
  Px,  ///< Pixels, 1px = 1/96th of 1in.
  /// @}
  /**
   * @addtogroup _RELATIVE
   * Relative lengths, https://www.w3.org/TR/css-values-3/#relative-lengths.
   *
   * @{
   */
  Em,    ///< Font size, 1em = current font size.
  Ex,    ///< x-height of the current font, 1ex = x-height of current font.
  Ch,    ///< Width of the glyph '0' in the current font, 1ch = width of '0' in current font.
  Rem,   ///< Root font size, 1rem = font size of the root element.
  Vw,    ///< Viewport width, 1vw = 1% of viewport width.
  Vh,    ///< Viewport height, 1vh = 1% of viewport height.
  Vmin,  ///< Smaller of viewport width and height, 1vmin = 1% of smaller of viewport width and
         ///< height.
  Vmax,  ///< Larger of viewport width and height, 1vmax = 1% of larger of viewport width and
         ///< height.
  /// @}
};

/// OStream output operator, writes the enum value as a stream, e.g. `Percent` or `Px`.
inline std::ostream& operator<<(std::ostream& os, LengthUnit unit) {
  switch (unit) {
    case LengthUnit::None: return os << "None";
    case LengthUnit::Percent: return os << "Percent";
    case LengthUnit::Cm: return os << "Cm";
    case LengthUnit::Mm: return os << "Mm";
    case LengthUnit::Q: return os << "Q";
    case LengthUnit::In: return os << "In";
    case LengthUnit::Pc: return os << "Pc";
    case LengthUnit::Pt: return os << "Pt";
    case LengthUnit::Px: return os << "Px";
    case LengthUnit::Em: return os << "Em";
    case LengthUnit::Ex: return os << "Ex";
    case LengthUnit::Ch: return os << "Ch";
    case LengthUnit::Rem: return os << "Rem";
    case LengthUnit::Vw: return os << "Vw";
    case LengthUnit::Vh: return os << "Vh";
    case LengthUnit::Vmin: return os << "Vmin";
    case LengthUnit::Vmax: return os << "Vmax";
  }

  UTILS_UNREACHABLE();
}

/**
 * Parses a CSS `<length-percentage>` type as defined by
 * https://www.w3.org/TR/css-values-3/#typedef-length-percentage
 *
 * A length is composed of a number followed by a unit identifier.
 *
 * Unit identifiers are split into two categories, absolute and relative:
 * - Absolute: https://www.w3.org/TR/css-values-3/#absolute-lengths
 *   - `cm`, `mm`, `Q`, `in`, `pc`, `pt`, and `px`
 * - Relative: https://www.w3.org/TR/css-values-3/#relative-lengths
 *   - `em`, `ex`, `ch`, `rem`, `vw`, `vh`, `vmin`, `vmax`
 *
 * The unit may be omitted for '0', which is unitless.
 *
 * For a percentage, the number is followed by the '\%' character.
 *
 * Examples:
 *  - `0`
 *  - `10px`
 *  - `50%`
 *
 * @tparam T Value storage type, typically double.
 */
template <typename T>
struct Length {
  /// The unit identifier for the length.
  using Unit = LengthUnit;

  /// The numeric value of the length.
  T value = T(0);
  /// The unit identifier of the length.
  Unit unit = Unit::None;

  /// Default constructor, initializes to unitless 0.
  Length() = default;

  /**
   * Construct a length from a value and unit.
   *
   * @param value The numeric value of the length.
   * @param unit The unit identifier of the length.
   */
  explicit Length(T value, Unit unit = Unit::None) : value(value), unit(unit) {}

  /// Equality operator, using near-equals comparison for the value.
  bool operator==(const Length& other) const {
    if (unit != other.unit) {
      return false;
    }

    return NearEquals(value, other.value);
  }

  /// Spaceship operator, first ordered by the unit if they are not the same, then by the value
  /// (with a near-equals comparison).
  std::partial_ordering operator<=>(const Length& other) const {
    if (unit != other.unit) {
      return std::partial_ordering(unit <=> other.unit);
    }
    if (NearEquals(value, other.value)) {
      return std::partial_ordering::equivalent;
    }
    return value <=> other.value;
  }

  /// Returns true if the length is an absolute dimension (not a percentage or relative unit).
  bool isAbsoluteSize() const {
    return unit == Unit::None || unit == Unit::Cm || unit == Unit::Mm || unit == Unit::Q ||
           unit == Unit::In || unit == Unit::Pc || unit == Unit::Pt || unit == Unit::Px;
  }

  /**
   * Selects which extent of the viewbox to use for percentage and viewbox-relative length
   * conversions, see \ref toPixels().
   */
  enum class Extent {
    X,     ///< Use X component of viewbox for percentage calculations.
    Y,     ///< Use Y component of viewbox for percentage calculations.
    Mixed  ///< Use diagonal extent of viewbox.
  };

  /**
   * Convert the length to pixels, following the ratios at
   * https://www.w3.org/TR/css-values/#absolute-lengths and
   * https://www.w3.org/TR/css-values/#relative-lengths.
   *
   * @param viewbox Viewbox of the element for computing viewbox-relative conversions.
   * @param fontMetrics Font size information for font-relative sizes.
   * @param extent Which extent of the viewbox to use for percentage and viewbox-relative length.
   * @return Length in pixels.
   */
  T toPixels(const Box<T>& viewbox, const FontMetrics& fontMetrics,
             Extent extent = Extent::Mixed) const {
    switch (unit) {
      // Absolute units.
      case Unit::None: return value;
      case Unit::Percent: {
        if (extent == Extent::X) {
          return value * viewbox.width() / 100;
        } else if (extent == Extent::Y) {
          return value * viewbox.height() / 100;
        } else {
          return value * diagonalExtent(viewbox) / 100;
        }
      }
      case Unit::Cm: return value * AbsoluteLengthMetrics::kCmToPixels;
      case Unit::Mm: return value * AbsoluteLengthMetrics::kCmToPixels / 10.0;
      case Unit::Q: return value * AbsoluteLengthMetrics::kCmToPixels / 40.0;
      case Unit::In: return value * AbsoluteLengthMetrics::kInchesToPixels;
      case Unit::Pc: return value * AbsoluteLengthMetrics::kInchesToPixels / 6.0;
      case Unit::Pt: return value * AbsoluteLengthMetrics::kPointsToPixels;
      case Unit::Px: return value;
      // Relative units.
      case Unit::Em: return value * fontMetrics.fontSize;
      case Unit::Ex: return value * fontMetrics.exUnit();
      case Unit::Ch: return value * fontMetrics.chUnit();
      case Unit::Rem: return value * fontMetrics.rootFontSize;
      case Unit::Vw: return value * viewbox.width() / 100.0;
      case Unit::Vh: return value * viewbox.height() / 100.0;
      case Unit::Vmin: return value * std::min(viewbox.width(), viewbox.height()) / 100.0;
      case Unit::Vmax: return value * std::max(viewbox.width(), viewbox.height()) / 100.0;
    }
  }

  /**
   * ostream-output operator for Length.
   *
   * @param os The ostream to write to.
   * @param length Length class to output.
   * @return The ostream that was written to, for chaining.
   */
  friend std::ostream& operator<<(std::ostream& os, const Length<T>& length) {
    return os << length.value << " " << length.unit;
  }

private:
  static T diagonalExtent(const Box<T>& box) {
    // Using the SVG spec's definition of normalized diagonal length:
    // > The normalized diagonal length must be calculated with
    // > `sqrt((width)**2 + (height)**2)/sqrt(2)`.
    // From https://svgwg.org/svg2-draft/coords.html#Units
    return box.size().length() * MathConstants<T>::kInvSqrt2;
  }
};

// Helper typedefs.
/// Shorthand for \ref Length<double>.
typedef Length<double> Lengthd;

}  // namespace donner
