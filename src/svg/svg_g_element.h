#pragma once
/// @file

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

/**
 * @page xml_g '<g>'
 * @ingroup elements_structural
 *
 * Creates a group of elements which can be transformed as a single object.
 *
 * - DOM object: SVGGElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#GElement
 *
 * ```xml
 * <g transform="translate(50 100)">
 *   <rect width="100" height="100" fill="black" />
 *   <rect x="50" y="50" width="100" height="100" fill="lime" />
 * </g>
 * ```
 *
 * \htmlonly
 * <svg id="xml_g" width="300" height="300" style="background-color: white">
 * <g transform="translate(50 100)">
 *   <rect width="100" height="100" fill="black" />
 *   <rect x="50" y="50" width="100" height="100" fill="lime" />
 * </g>
 * </svg>
 * \endhtmlonly
 */

/**
 * DOM object for a \ref xml_g element.
 */
class SVGGElement : public SVGGraphicsElement {
protected:
  /// Create an SVGGElement wrapper from an entity.
  explicit SVGGElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::G;

  /// XML tag name, \ref xml_g.
  static constexpr std::string_view Tag{"g"};

  /**
   * Create a new \ref xml_g element.
   *
   * @param document Containing document.
   */
  static SVGGElement Create(SVGDocument& document);
};

}  // namespace donner::svg
