#pragma once
/// @file

#include "src/css/color.h"

namespace donner::svg {

/**
 * Values for the "gradientUnits" attribute,
 * https://www.w3.org/TR/SVG2/pservers.html#LinearGradientElementGradientUnitsAttribute and
 * https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementGradientUnitsAttribute.
 *
 * This is used on `<linearGradient>` and `<radialGradient>` elements.
 *
 * - For linear gradients, this defines the coordinate system for attributes `x1`, `y1`, `x2`, and
 *   `y2`.
 * - For radial gradients, this defines the coordinate system for attributes `cx`, `cy`, `r`, `fx`,
 *   `fy`, and `fr`.
 */
enum class GradientUnits {
  UserSpaceOnUse,
  ObjectBoundingBox,
  Default = ObjectBoundingBox,
};

/**
 * Values for the gradient "spreadMethod" attribute,
 * https://www.w3.org/TR/SVG2/pservers.html#LinearGradientElementSpreadMethodAttribute and
 * https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementSpreadMethodAttribute.
 *
 * This is used on `<linearGradient>` and `<radialGradient>` elements.
 */
enum class GradientSpreadMethod {
  Pad,
  Reflect,
  Repeat,
  Default = Pad,
};

/**
 * Values for a gradient stop, https://www.w3.org/TR/SVG2/pservers.html#StopElement.
 *
 * This is used on `<stop>` elements.
 *
 * @related SVGGradientComponent
 */
struct GradientStop {
  float offset = 0.0;  ///< Offset of the gradient stop, in the range [0, 1].
  css::Color color{css::RGBA(0, 0, 0, 0xFF)};  ///< Color of the gradient stop.
  float opacity = 1.0f;  ///< Opacity of the gradient stop, in the range [0, 1].
};

}  // namespace donner::svg
