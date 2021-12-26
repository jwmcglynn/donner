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

  /**
   * Convert the length to pixels, following the ratios at
   * https://www.w3.org/TR/css-values/#absolute-lengths and
   * https://www.w3.org/TR/css-values/#relative-lengths.
   *
   * @param viewbox Viewbox of the element for computing viewbox-relative conversions.
   * @param fontMetrics Font size information for font-relative sizes.
   * @return T length in pixels.
   */
  T toPixels(const Box<T>& viewbox, const FontMetrics& fontMetrics) {
    switch (unit) {
      // Absolute units.
      case Unit::None: return value;
      case Unit::Percent: return value * diagonalExtent(viewbox) / 100.0;
      case Unit::Cm: return value * RelativeLengthMetrics::kCmToPixels;
      case Unit::Mm: return value * RelativeLengthMetrics::kCmToPixels / 10.0;
      case Unit::Q: return value * RelativeLengthMetrics::kCmToPixels / 40.0;
      case Unit::In: return value * RelativeLengthMetrics::kInchesToPixels;
      case Unit::Pc: return value * RelativeLengthMetrics::kInchesToPixels / 6.0;
      case Unit::Pt: return value * RelativeLengthMetrics::kInchesToPixels;
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

private:
  static T diagonalExtent(const Box<T>& box) { return box.size().length(); }
};

// Helper typedefs.
typedef Length<double> Lengthd;

}  // namespace donner
