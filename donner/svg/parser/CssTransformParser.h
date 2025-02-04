#pragma once
/// @file

#include <span>

#include "donner/base/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/svg/core/CssTransform.h"

namespace donner::svg::parser {

/**
 * Parse a CSS "transform" property.
 *
 * @see https://www.w3.org/TR/css-transforms-1/#transform-property
 */
class CssTransformParser {
public:
  /**
   * Parse a CSS "transform" property.
   *
   * @see https://www.w3.org/TR/css-transforms-1/#transform-property
   *
   *
   * Compared to the SVG `transform="..."` attribute, \ref TransformParser, this parser supports the
   * full CSS syntax including units on lengths and angles.
   *
   * Supported functions:
   * | Function                   | Description |
   * | --------------------------:| :---------- |
   * | `matrix(a, b, c, d, e, f)` | Applies a matrix transform defined by six numeric parameters. |
   * | `translate(x, y=0)`        | Translates by `(x, y)`. The values can include units (e.g. `px`, `%`, etc). |
   * | `translateX(x)`            | Translates along the X-axis by `x`. |
   * | `translateY(y)`            | Translates along the Y-axis by `y`. |
   * | `scale(x, y=x)`            | Scales by `(x, y)`. If `y` is omitted, the scale is uniform in both dimensions. |
   * | `scaleX(x)`                | Scales along the X-axis by `x`. |
   * | `scaleY(y)`                | Scales along the Y-axis by `y`. |
   * | `rotate(angle)`            | Rotates by `angle`. The angle may include units (e.g. `deg`, `rad`). |
   * | `skew(angle, theta=0)`     | Applies a skew transform with two angles. This function is deprecated in favor of `skewX`/`skewY`. |
   * | `skewX(angle)`             | Skews along the X-axis by `angle`. |
   * | `skewY(angle)`             | Skews along the Y-axis by `angle`. |
   *
   * In functions that accept multiple parameters, commas are required to separate the values.
   *
   *
   * @param components CSS ComponentValues for a parsed transform property.
   * @return Parsed CSS transform, or an error.
   */
  static ParseResult<CssTransform> Parse(std::span<const css::ComponentValue> components);
};

}  // namespace donner::svg::parser
