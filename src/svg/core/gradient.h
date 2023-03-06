#pragma once
/// @file

#include "src/css/color.h"

namespace donner::svg {

/**
 * Values for the `"gradientUnits"` attribute,
 * https://www.w3.org/TR/SVG2/pservers.html#LinearGradientElementGradientUnitsAttribute and
 * https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementGradientUnitsAttribute.
 *
 * This is used on \ref linearGradient and \ref radialGradient elements.
 *
 * - For linear gradients, this defines the coordinate system for attributes `x1`, `y1`, `x2`, and
 *   `y2`.
 * - For radial gradients, this defines the coordinate system for attributes `cx`, `cy`, `r`, `fx`,
 *   `fy`, and `fr`.
 */
enum class GradientUnits {
  /**
   * The gradient is defined in user space, which is the coordinate system of the element that
   * references the gradient.
   */
  UserSpaceOnUse,
  /**
   * The gradient is defined in object space, where (0, 0) is the top-left corner of the element
   * that references the gradient, and (1, 1) is the bottom-right corner. Note that this may result
   * in non-uniform scaling if the element is not square.
   */
  ObjectBoundingBox,
  /**
   * The default value for the `"gradientUnits"` attribute, which is `ObjectBoundingBox`.
   */
  Default = ObjectBoundingBox,
};

/**
 * Values for the gradient "spreadMethod" attribute,
 * https://www.w3.org/TR/SVG2/pservers.html#LinearGradientElementSpreadMethodAttribute and
 * https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementSpreadMethodAttribute.
 *
 * Specifies what happens at the start or end of a gradient, when the gradient coordinates are
 * inside the bounds of the referencing element.
 *
 * This is used on \ref linearGradient and \ref radialGradient elements.
 *
 * \htmlonly
 * <svg width="660" height="150">
 *   <style> text { text-anchor: middle; font-size: 16px; font-weight: bold } </style>
 *   <linearGradient id="GradientSpread" x1="0.4" x2="0.6">
 *     <stop offset="0%" stop-color="lightsalmon" />
 *     <stop offset="100%" stop-color="lightskyblue" />
 *   </linearGradient>
 *   <rect x="0" y="0" width="150" height="150" fill="url(#GradientSpread)" />
 *   <text x="75" y="140">Default</text>
 *
 *   <linearGradient id="GradientSpreadPad" href="#GradientSpread" spreadMethod="pad" />
 *   <rect x="160" y="0" width="150" height="150" fill="url(#GradientSpreadPad)" />
 *   <text x="235" y="140">Pad</text>
 *
 *   <linearGradient id="GradientSpreadReflect" href="#GradientSpread" spreadMethod="reflect" />
 *   <rect x="320" y="0" width="150" height="150" fill="url(#GradientSpreadReflect)" />
 *   <text x="395" y="140">Reflect</text>
 *
 *   <linearGradient id="GradientSpreadRepeat" href="#GradientSpread" spreadMethod="repeat" />
 *   <rect x="480" y="0" width="150" height="150" fill="url(#GradientSpreadRepeat)" />
 *   <text x="555" y="140">Repeat</text>
 * </svg>
 * \endhtmlonly
 */
enum class GradientSpreadMethod {
  Pad,            ///< The gradient is filled with the start or end color.
  Reflect,        ///< The gradient is reflected at the start or end.
  Repeat,         ///< The gradient is repeated at the start or end.
  Default = Pad,  ///< The default value for the `"spreadMethod"` attribute, which is `Pad`.
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
