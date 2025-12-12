#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/Transform.h"

namespace donner::svg::parser {

/**
 * Parse an SVG transform attribute, such as `translate(100 100)`, using the SVG syntax which does
 * not support units on numbers.
 *
 * @see https://www.w3.org/TR/css-transforms-1/#svg-transform
 */
class TransformParser {
public:
  /// Defines how angle values are interpreted while parsing.
  enum class AngleUnit {
    /// Interpret angles in degrees (SVG default).
    Degrees,
    /// Interpret angles in radians.
    Radians,
  };

  /// Configuration for parsing SVG transform strings.
  struct Options {
    /// Angle units used for `rotate`, `skewX`, and `skewY` parameters.
    AngleUnit angleUnit = AngleUnit::Degrees;
  };

  /**
   * Parse an SVG `transform="..."` attribute.
   *
   * @see https://www.w3.org/TR/css-transforms-1/#svg-transform
   *
   * Compared to the CSS transform attribute, \ref CssTransformParser, this parser does not support
   * units on numbers, and the default units are pixels and degrees.
   *
   * - `translate(100 100)` - translates by `(100, 100)` pixels.
   * - `rotate(45)` - rotates by 45 degrees.
   *
   * Supported functions:
   * | Function | Description |
   * | -------: | :---------- |
   * | `matrix(a, b, c, d, e, f)` | Matrix transform. \see \ref donner::Transform |
   * | `translate(x, y=0)` | Translates by `(x, y)` pixels. |
   * | `scale(x, y=x)` | Scales by `(x, y)`. |
   * | `rotate(angle)` | Rotates by `angle` degrees. |
   * | `rotate(angle, cx, cy)` | Rotates by `angle` degrees around `(cx, cy)`. |
   * | `skewX(angle)` | Skews by `angle` degrees along the X axis. |
   * | `skewY(angle)` | Skews by `angle` degrees along the Y axis. |
   *
   * Commas between parameters are optional, and multiple transform functions may be composed for
   * more complex transforms, such as `rotate(45) translate(100 100)`.
   *
   * @param str String corresponding to the SVG transform attribute.
   * @return Parsed transform, or an error.
   */
  static ParseResult<Transformd> Parse(std::string_view str,
                                       const Options& options = Options());
};

}  // namespace donner::svg::parser
