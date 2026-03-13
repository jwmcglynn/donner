#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_animateTransform "<animateTransform>"
 *
 * Animates a transform attribute of an element over time.
 *
 * - DOM object: SVGAnimateTransformElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#AnimateTransformElement
 *
 * The `type` attribute selects the transform function (translate, scale, rotate, skewX, skewY).
 *
 * Example usage:
 * ```xml
 * <rect width="100" height="100" fill="blue">
 *   <animateTransform attributeName="transform" type="rotate"
 *                     from="0 50 50" to="360 50 50" dur="3s" />
 * </rect>
 * ```
 */

/**
 * DOM object for a \ref xml_animateTransform element.
 */
class SVGAnimateTransformElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGAnimateTransformElement wrapper from an entity.
  explicit SVGAnimateTransformElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAnimateTransformElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::AnimateTransform;
  /// XML tag name, \ref xml_animateTransform.
  static constexpr std::string_view Tag{"animateTransform"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
