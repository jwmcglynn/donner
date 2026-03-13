#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_animateMotion "<animateMotion>"
 *
 * Moves an element along a motion path over time.
 *
 * - DOM object: SVGAnimateMotionElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#AnimateMotionElement
 *
 * The motion path is specified via the `path` attribute or implicitly via `from`/`to`/`by`/`values`.
 *
 * Example usage:
 * ```xml
 * <circle cx="0" cy="0" r="5" fill="red">
 *   <animateMotion path="M0,0 L100,0 L100,100" dur="3s" />
 * </circle>
 * ```
 */

/**
 * DOM object for a \ref xml_animateMotion element.
 */
class SVGAnimateMotionElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGAnimateMotionElement wrapper from an entity.
  explicit SVGAnimateMotionElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAnimateMotionElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::AnimateMotion;
  /// XML tag name, \ref xml_animateMotion.
  static constexpr std::string_view Tag{"animateMotion"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
