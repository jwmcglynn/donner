#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_set "<set>"
 *
 * Sets an attribute to a discrete value for the duration of an animation.
 *
 * - DOM object: SVGSetElement
 * - SVG Animations spec: https://svgwg.org/specs/animations/#SetElement
 *
 * `<set>` is the simplest SVG animation element. It sets a target attribute to a specified
 * value when the animation is active. Unlike `<animate>`, it does not interpolate between
 * values.
 *
 * Example usage:
 * ```xml
 * <rect width="100" height="100" fill="red">
 *   <set attributeName="fill" to="blue" begin="2s" dur="3s" />
 * </rect>
 * ```
 */

/**
 * DOM object for a \ref xml_set element.
 *
 * ```xml
 * <rect width="100" height="100" fill="red">
 *   <set attributeName="fill" to="blue" begin="2s" dur="3s" />
 * </rect>
 * ```
 */
class SVGSetElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGSetElement wrapper from an entity.
  explicit SVGSetElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGSetElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Set;
  /// XML tag name, \ref xml_set.
  static constexpr std::string_view Tag{"set"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
