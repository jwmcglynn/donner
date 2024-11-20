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
 * Each stop element defines an offset and a color. The offset is a percentage of the distance
 * between the start and end of the gradient.
 *
 * ```xml
 * <linearGradient id="MyGradient">
 *   <stop offset="0%" stop-color="blue" />
 *   <stop offset="100%" stop-color="yellow" />
 * </linearGradient>
 * ```
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
  static SVGStopElement Create(SVGDocument& document) { return CreateOn(CreateEntity(document)); }

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
