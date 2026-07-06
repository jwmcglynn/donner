#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_animate "<animate>"
 *
 * Animates an attribute of an element over time with interpolation.
 *
 * - DOM object: SVGAnimateElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#AnimateElement
 *
 * `<animate>` supports multiple interpolation modes (linear, discrete, paced, spline)
 * and can animate numeric values, colors, lengths, and other interpolable types.
 *
 * Example usage:
 * ```xml
 * <rect width="100" height="100" fill="red">
 *   <animate attributeName="opacity" from="1" to="0" dur="2s" />
 * </rect>
 * ```
 */

/**
 * DOM object for a \ref xml_animate element.
 *
 * ```xml
 * <circle cx="50" cy="50" r="40">
 *   <animate attributeName="cx" from="50" to="200" dur="3s" fill="freeze" />
 * </circle>
 * ```
 */
class SVGAnimateElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGAnimateElement wrapper from an entity.
  explicit SVGAnimateElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAnimateElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Animate;
  /// XML tag name, \ref xml_animate.
  static constexpr std::string_view Tag{"animate"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
