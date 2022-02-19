#pragma once

#include "src/base/box.h"
#include "src/base/relative_length_metrics.h"

namespace donner {

/**
 * Parses a CSS <length-percentage> type as defined by
 * https://www.w3.org/TR/css-values-3/#typedef-length-percentage
 *
 * A length is composed of a number followed by a unit identifier.
 *
 * Unit identifiers are split into two categories, absolute and relative:
 *
 *  Absolute: https://www.w3.org/TR/css-values-3/#absolute-lengths
 *    cm, mm, Q, in, pc, pt, and px
 *
 *  Relative: https://www.w3.org/TR/css-values-3/#relative-lengths
 *    em, ex, ch, rem, vw, vh, vmin, vmax
 *
 * The unit may be omitted for '0', which is unitless.
 *
 * For a percentage, the number is followed by the '%' character.
 *
 * Examples:
 *  0
 *  10px
 *  50%
 *
 * @tparam T Value storage type, typically double.
 */
template <typename T>
struct Length {
  enum class Unit {
    None,
    Percent,
    // Absolute
    Cm,
    Mm,
    Q,
    In,
    Pc,
    Pt,
    Px,
    // Relative
    Em,
    Ex,
    Ch,
    Rem,
    Vw,
    Vh,
    Vmin,
    Vmax,
  };

  T value = T(0);
  Unit unit = Unit::None;

  Length() = default;
  explicit Length(T value, Unit unit = Unit::None) : value(value), unit(unit) {}

  enum class Extent {
    X,     //!< Use X component of viewbox for percentage calculations.
    Y,     //!< Use Y component of viewbox for percentage calculations.
    Mixed  //!< Use diagonal extent of viewbox.
  };

  /**
   * Convert the length to pixels, following the ratios at
   * https://www.w3.org/TR/css-values/#absolute-lengths and
   * https://www.w3.org/TR/css-values/#relative-lengths.
   *
   * @param viewbox Viewbox of the element for computing viewbox-relative conversions.
   * @param fontMetrics Font size information for font-relative sizes.
   * @return T length in pixels.
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
      case Unit::Cm: return value * RelativeLengthMetrics::kCmToPixels;
      case Unit::Mm: return value * RelativeLengthMetrics::kCmToPixels / 10.0;
      case Unit::Q: return value * RelativeLengthMetrics::kCmToPixels / 40.0;
      case Unit::In: return value * RelativeLengthMetrics::kInchesToPixels;
      case Unit::Pc: return value * RelativeLengthMetrics::kInchesToPixels / 6.0;
      case Unit::Pt: return value * RelativeLengthMetrics::kPointsToPixels;
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
typedef Length<double> Lengthd;

}  // namespace donner
