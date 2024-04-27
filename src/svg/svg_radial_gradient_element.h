#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/svg_gradient_element.h"

namespace donner::svg {

/**
 * @defgroup xml_radialGradient '<radialGradient>'
 *
 * Defines the paint server for a radial gradients.
 *
 * - DOM object: SVGRadialGradientElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#RadialGradients
 *
 * These elements are typically placed within a \ref xml_defs element, and then referenced by id
 * from a `fill` or `stroke` attribute.
 *
 * ```xml
 * <radialGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </radialGradient>
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
 *     <radialGradient id="MyGradient">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </radialGradient>
 *   </defs>
 *   <rect fill="url(#MyGradient)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * Valid child elements: \ref xml_stop
 *
 * @see \ref xml_linearGradient, \ref xml_stop
 */

/**
 * DOM object for a \ref xml_radialGradient element.
 *
 * ```xml
 * <radialGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </radialGradient>
 * ```
 *
 * To reference it with a fill:
 * ```xml
 * <rect fill="url(#MyGradient)" width="300" height="300" />
 * ```
 *
 * \htmlonly
 * <svg width="300" height="300">
 *  <defs>
 *     <radialGradient id="MyGradient">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </radialGradient>
 *   </defs>
 *   <rect fill="url(#MyGradient)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * To control the shape of the radial gradient, use the `cx`, `cy`, `r`, `fx`, `fy`, and `fr`
 * attributes, which defines two circles representing the start and end of the gradient, the focal
 * point and the outer circle. If the focal point is not specified (`fx`, `fy`, and `fr`), the focal
 * point defaults to the position of the outer circle (`cx` and `cy`) with radius `0`.
 *
 * \htmlonly
 * <svg width="300" height="300">
 *   <style>
 *     text { font-size: 16px; font-weight: bold; color: black }
 *     line { stroke: black; stroke-width: 2px; stroke-dasharray: 6,4 }
 *     circle { stroke-width: 2px }
 *   </style>
 *   <defs>
 *     <radialGradient id="ExampleGradient" cx="150" cy="150" r="140" fx="100" fy="200" fr="50"
 * gradientUnits="userSpaceOnUse">
 *       <stop offset="0%" stop-color="#afa" />
 *       <stop offset="100%" stop-color="#fac" />
 *     </radialGradient>
 *   </defs>
 *   <rect fill="url(#ExampleGradient)" width="300" height="300" />
 *
 *   <circle cx="150" cy="150" r="140" fill="none" stroke="black" />
 *   <circle cx="150" cy="150" r="3" fill="black" />
 *   <text x="160" y="153">cx,cy</text>
 *   <line x1="150" y1="150" x2="150" y2="10" stroke="black" />
 *   <text x="160" y="80">r</text>
 *
 *   <circle cx="100" cy="200" r="50" fill="none" stroke="black" />
 *   <circle cx="100" cy="200" r="3" fill="black" />
 *   <text x="110" y="203">fx,fy</text>
 *   <line x1="100" y1="200" x2="100" y2="250" stroke="black" />
 *   <text x="110" y="230">fr</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `cx`      | `50%`   | Center X coordinate, for the outer circle. |
 * | `cy`      | `50%`   | Center Y coordinate, for the outer circle. |
 * | `r`       | `50%`   | Radius of the outer circle. |
 * | `fx`      | `cx`    | Focal point X coordinate. |
 * | `fy`      | `cy`    | Focal point Y coordinate. |
 * | `fr`      | 0       | Focal point radius. |
 *
 * @see \ref SVGLinearGradientElement, \ref SVGStopElement
 */
class SVGRadialGradientElement : public SVGGradientElement {
protected:
  /// Create an SVGRadialGradientElement wrapper from an entity.
  explicit SVGRadialGradientElement(EntityHandle handle) : SVGGradientElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::RadialGradient;
  /// XML tag name, \ref xml_radialGradient.
  static constexpr XMLQualifiedNameRef Tag{"radialGradient"};

  /**
   * Create a new \ref xml_radialGradient element.
   *
   * @param document Containing document.
   */
  static SVGRadialGradientElement Create(SVGDocument& document);

  /**
   * Set the center X coordinate, for the outer circle.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setCx(std::optional<Lengthd> value);

  /**
   * Set the center Y coordinate, for the outer circle.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setCy(std::optional<Lengthd> value);

  /**
   * Set the radius of the outer circle.
   *
   * @param value Radius value, or `std::nullopt` to remove the attribute.
   */
  void setR(std::optional<Lengthd> value);

  /**
   * Set the focal point X coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setFx(std::optional<Lengthd> value);

  /**
   * Set the focal point Y coordinate.
   *
   * @param value Coordinate value, or `std::nullopt` to remove the attribute.
   */
  void setFy(std::optional<Lengthd> value);

  /**
   * Set the focal point radius.
   *
   * @param value Radius value, or `std::nullopt` to remove the attribute.
   */
  void setFr(std::optional<Lengthd> value);

  /**
   * Get the center X coordinate, for the outer circle. Note that at render-time, this will default
   * to `50%` if not set.
   */
  std::optional<Lengthd> cx() const;

  /**
   * Get the center Y coordinate, for the outer circle. Note that at render-time, this will default
   * to `50%` if not set.
   */
  std::optional<Lengthd> cy() const;

  /**
   * Get the radius of the outer circle. Note that at render-time, this will default to `50%` if not
   * set.
   */
  std::optional<Lengthd> r() const;

  /**
   * Get the focal point X coordinate. Note that at render-time, this will default to `cx()` if not
   * set.
   */
  std::optional<Lengthd> fx() const;

  /**
   * Get the focal point Y coordinate. Note that at render-time, this will default to `cy()` if not
   * set.
   */
  std::optional<Lengthd> fy() const;

  /**
   * Get the focal point radius. Note that at render-time, this will default to `0` if not set.
   */
  std::optional<Lengthd> fr() const;
};

}  // namespace donner::svg
