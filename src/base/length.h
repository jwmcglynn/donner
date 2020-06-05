#pragma once

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
};

// Helper typedefs.
typedef Length<double> Lengthd;

}  // namespace donner
