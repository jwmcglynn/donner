#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGGradientElement.h"

namespace donner::svg {

/**
 * @page xml_radialGradient "<radialGradient>"
 *
 * Defines the paint server for a radial gradients.
 *
 * - DOM object: SVGRadialGradientElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#RadialGradients
 *
 * A `<radialGradient>` is a **paint server** that interpolates colors outward from a focal
 * point to an outer circle, producing effects like spotlights, glows, and soft spherical
 * shading. Like \ref xml_linearGradient, it does not draw anything itself; you declare it with
 * an `id` (usually inside \ref xml_defs) and reference it from another shape's `fill` or
 * `stroke` via `url(#id)`. Its child \ref xml_stop elements define the color ramp, and SVG
 * smoothly interpolates between them based on each pixel's distance from the focal point to
 * the outer circle.
 *
 * For a straight-line color ramp, use \ref xml_linearGradient instead.
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
 * <svg id="xml_radialGradient_basic" width="300" height="300">
 *   <defs>
 *     <radialGradient id="xml_radialGradient_basic_grad">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </radialGradient>
 *   </defs>
 *   <rect fill="url(#xml_radialGradient_basic_grad)" width="300" height="300" />
 * </svg>
 * \endhtmlonly
 *
 * # Focal point (fx, fy, fr)
 *
 * By default a radial gradient is symmetric: the colors radiate from the center `(cx, cy)` out
 * to the outer circle of radius `r`. Offsetting the focal point with `fx`/`fy` shifts the "inner
 * circle" of the gradient, which is how you get a lens-like or spotlight effect — the 0% color
 * appears at `(fx, fy)` instead of the geometric center, but the 100% color still lives on the
 * outer circle at `(cx, cy)` with radius `r`. `fr` lets the inner circle have its own radius
 * greater than 0, creating an annular (ring-shaped) gradient.
 *
 * \htmlonly
 * <svg id="xml_radialGradient_focal" width="400" height="160" viewBox="0 0 400 160" style="background-color: white" font-family="sans-serif" font-size="11">
 *   <defs>
 *     <radialGradient id="xml_radialGradient_focal_centered" cx="50%" cy="50%" r="50%">
 *       <stop offset="0%" stop-color="white" />
 *       <stop offset="100%" stop-color="#1f5a8a" />
 *     </radialGradient>
 *     <radialGradient id="xml_radialGradient_focal_offset" cx="50%" cy="50%" r="50%" fx="30%" fy="30%">
 *       <stop offset="0%" stop-color="white" />
 *       <stop offset="100%" stop-color="#1f5a8a" />
 *     </radialGradient>
 *     <radialGradient id="xml_radialGradient_focal_ring" cx="50%" cy="50%" r="50%" fx="50%" fy="50%" fr="25%">
 *       <stop offset="0%" stop-color="white" />
 *       <stop offset="100%" stop-color="#1f5a8a" />
 *     </radialGradient>
 *   </defs>
 *   <text x="70"  y="15" text-anchor="middle" font-weight="bold">default (centered)</text>
 *   <circle cx="70"  cy="80" r="55" fill="url(#xml_radialGradient_focal_centered)" stroke="#555" />
 *   <text x="70"  y="150" text-anchor="middle" font-family="monospace" font-size="10">fx=cx fy=cy fr=0</text>
 *   <text x="200" y="15" text-anchor="middle" font-weight="bold">offset focal point</text>
 *   <circle cx="200" cy="80" r="55" fill="url(#xml_radialGradient_focal_offset)" stroke="#555" />
 *   <text x="200" y="150" text-anchor="middle" font-family="monospace" font-size="10">fx=30% fy=30%</text>
 *   <text x="330" y="15" text-anchor="middle" font-weight="bold">annular (fr > 0)</text>
 *   <circle cx="330" cy="80" r="55" fill="url(#xml_radialGradient_focal_ring)" stroke="#555" />
 *   <text x="330" y="150" text-anchor="middle" font-family="monospace" font-size="10">fr=25%</text>
 * </svg>
 * \endhtmlonly
 *
 * # gradientUnits
 *
 * Like `<pattern>`, `gradientUnits` decides whether `cx`, `cy`, `r`, `fx`, `fy`, and `fr` are
 * interpreted as **fractions of the filled shape's bounding box** (`objectBoundingBox`, the
 * default) or **absolute user-space coordinates** (`userSpaceOnUse`). In the default mode a
 * single gradient definition automatically adapts to any shape's size; in `userSpaceOnUse`
 * mode the gradient's size and position are fixed in page coordinates regardless of which
 * shape it fills.
 *
 * Valid child elements: \ref xml_stop
 *
 * @see \ref xml_linearGradient, \ref xml_stop
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `cx`      | `50%`   | Center X coordinate, for the outer circle. |
 * | `cy`      | `50%`   | Center Y coordinate, for the outer circle. |
 * | `r`       | `50%`   | Radius of the outer circle. |
 * | `fx`      | `cx`    | Focal point X coordinate. |
 * | `fy`      | `cy`    | Focal point Y coordinate. |
 * | `fr`      | `0`     | Focal point radius. |
 * | `gradientUnits` | `objectBoundingBox` | The coordinate system for the gradient, either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `gradientTransform` | (none) | A transform to apply to the gradient. |
 * | `spreadMethod` | `pad` | How to handle colors outside the gradient. Either `pad`, `reflect`, or `repeat`. |
 * | `href`    | (none)  | A URL reference to a template gradient element, which is then used as a template for this gradient. Example: `<radialGradient id="MyGradient" href="#MyGradient2" />` |
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
 * | `fr`      | `0`       | Focal point radius. |
 * | `gradientUnits` | `objectBoundingBox` | The coordinate system for the gradient, either `userSpaceOnUse` or `objectBoundingBox`. |
 * | `gradientTransform` | (none) | A transform to apply to the gradient. |
 * | `spreadMethod` | `pad` | How to handle colors outside the gradient. Either `pad`, `reflect`, or `repeat`. |
 * | `href`    | (none)  | A URL reference to a template gradient element, which is then used as a template for this gradient. Example: `<radialGradient id="MyGradient" href="#MyGradient2" />` |
 *
 * @see \ref SVGLinearGradientElement, \ref SVGStopElement
 */
class SVGRadialGradientElement : public SVGGradientElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGRadialGradientElement wrapper from an entity.
  explicit SVGRadialGradientElement(EntityHandle handle) : SVGGradientElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGRadialGradientElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::RadialGradient;
  /// XML tag name, \ref xml_radialGradient.
  static constexpr std::string_view Tag{"radialGradient"};

  static_assert(SVGGradientElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_radialGradient element.
   *
   * @param document Containing document.
   */
  static SVGRadialGradientElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

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
