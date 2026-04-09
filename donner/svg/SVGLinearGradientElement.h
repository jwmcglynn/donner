#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/SVGGradientElement.h"

namespace donner::svg {

/**
 * @page xml_linearGradient "<linearGradient>"
 *
 * Defines the paint server for a linear gradients.
 *
 * - DOM object: SVGLinearGradientElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#LinearGradients
 *
 * A `<linearGradient>` is a **paint server**: it does not draw anything on its own, but
 * describes a color ramp that other shapes can use as their `fill` or `stroke`. The ramp
 * interpolates smoothly between a list of \ref xml_stop child elements along a straight line
 * from `(x1, y1)` to `(x2, y2)`. At render time, every pixel of the filled shape is colored
 * by projecting it onto that line and looking up the interpolated stop color at that position.
 *
 * Gradients are almost always declared inside a \ref xml_defs block with an `id`, then
 * referenced by that id from a paint attribute like `fill="url(#MyGradient)"`. For a radial
 * (circular) color ramp instead of a linear one, use \ref xml_radialGradient.
 *
 * ```xml
 * <linearGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </linearGradient>
 * ```
 *
 * To reference it with a fill:
 * ```xml
 * <rect fill="url(#MyGradient)" width="300" height="300" />
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300">
 *   <defs>
 *     <linearGradient id="MyGradient">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </linearGradient>
 *   </defs>
 *   <rect fill="url(#MyGradient)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * Valid child elements: \ref xml_stop
 *
 * @see \ref xml_radialGradient, \ref xml_stop
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x1`      | `0%`    | Start X coordinate. |
 * | `y1`      | `0%`    | Start Y coordinate. |
 * | `x2`      | `100%`  | End X coordinate. |
 * | `y2`      | `100%`  | End Y coordinate. |
 * | `gradientUnits` | `objectBoundingBox` | The coordinate system for the gradient, either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `gradientTransform` | (none) | A transform to apply to the gradient. |
 * | `spreadMethod` | `pad` | How to handle colors outside the gradient. Either `pad`, `reflect`, or `repeat`. |
 * | `href`    | (none)  | A URL reference to a template gradient element, which is then used as a template for this gradient. Example: `<linearGradient id="MyGradient" href="#MyGradient2" />` |
 */

/**
 * DOM object for a \ref xml_linearGradient element.
 *
 * ```xml
 * <linearGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </linearGradient>
 * ```
 *
 * To reference it with a fill:
 * ```xml
 * <rect fill="url(#MyGradient)" width="300" height="300" />
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300">
 *   <defs>
 *     <linearGradient id="MyGradient">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </linearGradient>
 *   </defs>
 *   <rect fill="url(#MyGradient)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * To control the direction of the gradient, use the `x1`, `y1`, `x2`, and `y2` attributes, which
 * define a line that defines the start and stop of the gradient.
 *
 * \htmlonly
 * <svg width="300" height="300">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *   </style>
 *   <defs>
 *     <linearGradient id="ExampleGradient" x1="100" y1="100" x2="200" y2="200" gradientUnits="userSpaceOnUse">
 *       <stop offset="0%" stop-color="#afa" />
 *       <stop offset="100%" stop-color="#fac" />
 *     </linearGradient>
 *   </defs>
 *   <rect fill="url(#ExampleGradient)" width="300" height="300" />
 *
 *   <line x1="100" y1="100" x2="200" y2="200" stroke="black" stroke-width="2" />
 *
 *   <circle cx="100" cy="100" r="3" fill="black" />
 *   <text x="110" y="103">x1,y1</text>
 *
 *   <circle cx="200" cy="200" r="3" fill="black" />
 *   <text x="210" y="203">x2,y2</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `x1`      | `0%`    | Start X coordinate. |
 * | `y1`      | `0%`    | Start Y coordinate. |
 * | `x2`      | `100%`  | End X coordinate. |
 * | `y2`      | `100%`  | End Y coordinate. |
 * | `gradientUnits` | `objectBoundingBox` | The coordinate system for the gradient, either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `gradientTransform` | (none) | A transform to apply to the gradient. |
 * | `spreadMethod` | `pad` | How to handle colors outside the gradient. Either `pad`, `reflect`, or `repeat`. |
 * | `href`    | (none)  | A URL reference to a template gradient element, which is then used as a template for this gradient. Example: `<linearGradient id="MyGradient" href="#MyGradient2" />` |
 *
 * @see \ref SVGRadialGradientElement, \ref SVGStopElement
 */
class SVGLinearGradientElement : public SVGGradientElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGLinearGradientElement wrapper from an entity.
  explicit SVGLinearGradientElement(EntityHandle handle) : SVGGradientElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGLinearGradientElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::LinearGradient;
  /// XML tag name, \ref xml_linearGradient.
  static constexpr std::string_view Tag{"linearGradient"};

  static_assert(SVGGradientElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_linearGradient element.
   *
   * @param document Containing document.
   */
  static SVGLinearGradientElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the start X coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setX1(std::optional<Lengthd> value);

  /**
   * Set the start Y coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setY1(std::optional<Lengthd> value);

  /**
   * Set the end X coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setX2(std::optional<Lengthd> value);

  /**
   * Set the end Y coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setY2(std::optional<Lengthd> value);

  /**
   * Get the start Y coordinate. Note that at render-time, this will default to `0%` if not set.
   */
  std::optional<Lengthd> x1() const;

  /**
   * Get the start Y coordinate. Note that at render-time, this will default to `0%` if not set.
   */
  std::optional<Lengthd> y1() const;

  /**
   * Get the end X coordinate. Note that at render-time, this will default to `100%` if not set.
   */
  std::optional<Lengthd> x2() const;

  /**
   * Get the end Y coordinate. Note that at render-time, this will default to `100%` if not set.
   */
  std::optional<Lengthd> y2() const;
};

}  // namespace donner::svg
