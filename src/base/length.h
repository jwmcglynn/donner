#pragma once
/// @file

#include "src/base/box.h"
#include "src/base/relative_length_metrics.h"

namespace donner {

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
  /**
   * The unit identifier for a length, corresponding to CSS unit identifiers.
   * See https://www.w3.org/TR/css-values-3/#lengths for definitions.
   */
  enum class Unit {
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

  /**
   * Selects which extent of the viewbox to use for percentage and viewbox-relative length
   * conversions, see \ref toPixels().
   */
  enum class Extent {
    X,     ///< Use X component of viewbox for percentage calculations.
    Y,     ///< Use Y component of viewbox for percentage calculations.
    Mixed  ///< Use diagonal extent of viewbox.
  };

  /// Returns true if the length is an absolute dimension (not a percentage or relative unit).
  bool isAbsoluteSize() const {
    return unit == Unit::None || unit == Unit::Cm || unit == Unit::Mm || unit == Unit::Q ||
           unit == Unit::In || unit == Unit::Pc || unit == Unit::Pt || unit == Unit::Px;
  }

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
    os << length.value;
    switch (length.unit) {
      case Unit::None: break;
      case Unit::Percent: os << "%"; break;
      case Unit::Cm: os << "cm"; break;
      case Unit::Mm: os << "mm"; break;
      case Unit::Q: os << "Q"; break;
      case Unit::In: os << "in"; break;
      case Unit::Pc: os << "pc"; break;
      case Unit::Pt: os << "pt"; break;
      case Unit::Px: os << "px"; break;
      case Unit::Em: os << "em"; break;
      case Unit::Ex: os << "ex"; break;
      case Unit::Ch: os << "ch"; break;
      case Unit::Rem: os << "rem"; break;
      case Unit::Vw: os << "vw"; break;
      case Unit::Vh: os << "vh"; break;
      case Unit::Vmin: os << "vmin"; break;
      case Unit::Vmax: os << "vmax"; break;
    }
    return os;
  }

private:
  static T diagonalExtent(const Box<T>& box) {
    // Using the SVG spec's definition of normalized diagonal length:
    // > The normalized diagonal length must be calculated with
    // > `sqrt((width)**2 + (height)**2)/sqrt(2)`.
    // From https://svgwg.org/svg2-draft/coords.html#Units
    constexpr T kInvSqrt2(0.70710678118);
    return box.size().length() * kInvSqrt2;
  }
};

// Helper typedefs.
/// Shorthand for \ref Length<double>.
typedef Length<double> Lengthd;

}  // namespace donner
