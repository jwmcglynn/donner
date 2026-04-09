#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_stop "<stop>"
 *
 * Defines a color stop for a gradient. This is a child element of \ref
 * xml_linearGradient and \ref xml_radialGradient.
 *
 * - DOM object: SVGStopElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/pservers.html#StopElement
 *
 * A `<stop>` is a single control point along a gradient's color ramp. Each stop has three
 * pieces of information: an `offset` in the range `[0, 1]` (where `0` is the start of the
 * gradient and `1` is the end), a `stop-color`, and an optional `stop-opacity`. At render time,
 * SVG smoothly interpolates color and opacity between adjacent stops, so a gradient with two
 * stops produces a straight fade and a gradient with many stops can produce complex
 * multi-color transitions. Stops only make sense as children of \ref xml_linearGradient or
 * \ref xml_radialGradient.
 *
 * ```xml
 * <linearGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="50%" stop-color="white" />
 *   <stop offset="100%" stop-color="yellow" />
 * </linearGradient>
 * ```
 *
 * The diagram below shows a linear gradient filled rectangle with three stops. Each stop is
 * annotated with its `offset` and `stop-color`, and a dashed guide line shows where along the
 * gradient axis the stop is positioned.
 *
 * \htmlonly
 * <svg id="xml_stop" width="320" height="160" style="background-color: white">
 *   <style>
 *     #xml_stop text { font-size: 13px; font-weight: bold; fill: black }
 *     #xml_stop line { stroke: black; stroke-width: 1.5; stroke-dasharray: 5,3 }
 *     #xml_stop circle { r: 4; fill: black }
 *   </style>
 *   <defs>
 *     <linearGradient id="xml_stop_grad" x1="0" y1="0" x2="1" y2="0">
 *       <stop offset="0%" stop-color="blue" />
 *       <stop offset="50%" stop-color="white" />
 *       <stop offset="100%" stop-color="yellow" />
 *     </linearGradient>
 *   </defs>
 *
 *   <rect x="20" y="40" width="280" height="60" fill="url(#xml_stop_grad)" stroke="black" />
 *
 *   <circle cx="20"  cy="70" />
 *   <line x1="20"  y1="40" x2="20"  y2="120" />
 *   <text x="4"  y="140">0%</text>
 *   <text x="4"  y="155">blue</text>
 *
 *   <circle cx="160" cy="70" />
 *   <line x1="160" y1="40" x2="160" y2="120" />
 *   <text x="140" y="140">50%</text>
 *   <text x="140" y="155">white</text>
 *
 *   <circle cx="300" cy="70" />
 *   <line x1="300" y1="40" x2="300" y2="120" />
 *   <text x="270" y="140">100%</text>
 *   <text x="272" y="155">yellow</text>
 * </svg>
 * \endhtmlonly
 *
 * | Attribute      | Default | Description  |
 * | -------------: | :-----: | :----------- |
 * | `offset`       | `0`     | Where the gradient stop is placed, in the range of `[0, 1]` (or `0%` to `100%`). |
 * | `stop-color`   | `black` | Color of the gradient stop. |
 * | `stop-opacity` | `1`     | Opacity of the gradient stop, in the range of `[0, 1]`. |
 */

/**
 * DOM object for a \ref xml_stop element.
 *
 * ```xml
 * <linearGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </linearGradient>
 * ```
 *
 * This is used as a child element of \ref SVGLinearGradientElement and \ref
 * SVGRadialGradientElement, to specify the vector of colors that will be used to fill the gradient.
 *
 * | Attribute | Default | Description  |
 * | --------: | :-----: | :----------- |
 * | `offset`  | `0` | Where the gradient stop is placed, in the range of [0, 1]. |
 * | `stop-color` | `black` | Color of the gradient stop. |
 * | `stop-opacity` | `1` | Opacity of the gradient stop. |
 *
 * @see \ref SVGLinearGradientElement, \ref SVGRadialGradientElement
 */
class SVGStopElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGStopElement wrapper from an entity.
  explicit SVGStopElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGStopElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Stop;
  /// XML tag name, \ref xml_stop.
  static constexpr std::string_view Tag{"stop"};

  /**
   * Create a new \ref xml_stop element.
   *
   * @param document Containing document.
   */
  static SVGStopElement Create(SVGDocument& document) {
    return CreateOn(CreateEmptyEntity(document));
  }

  /**
   * Set the offset of the gradient stop, within the range of `[0, 1]`.
   *
   * @param value Offset of the gradient stop.
   * @pre \p value Must be in the range of `[0, 1]`, otherwise it will assert.
   */
  void setOffset(float value);

  /**
   * Set the color of the gradient stop.
   *
   * @param value Color of the gradient stop.
   */
  void setStopColor(css::Color value);

  /**
   * Set the opacity of the gradient stop.
   *
   * @param value Opacity of the gradient stop.
   * @pre \p value Must be in the range of `[0, 1]`, otherwise it will assert.
   */
  void setStopOpacity(double value);

  /**
   * Get the offset of the gradient stop on the element, within the range of `[0, 1]`.
   */
  float offset() const;

  /**
   * Get the color of the gradient stop on the element.
   */
  css::Color stopColor() const;

  /**
   * Get the opacity of the gradient stop on the element, within the range of `[0, 1]`.
   */
  double stopOpacity() const;

  // NOTE: offset is not a presentation property, so it is not different when computed.

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `stop-color`
   * property.
   *
   * This will also resolve the `currentColor` keyword.
   */
  css::Color computedStopColor() const;

  /**
   * Applies stylesheet rules to the element, and returns the computed value of the `stop-opacity`
   * property.
   */
  double computedStopOpacity() const;

private:
  /// Invalidates cached data from the render tree.
  void invalidate() const;
};

}  // namespace donner::svg
