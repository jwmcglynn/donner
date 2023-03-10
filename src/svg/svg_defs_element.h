#pragma once
/// @file

#include "src/svg/svg_graphics_element.h"

namespace donner::svg {

/**
 * @defgroup xml_defs '<defs>'
 *
 * The `<defs>` element is used to define reusable graphics elements. It is not rendered directly,
 * but its child elements can be referenced by a \ref xml_use or within a `fill` or `stroke`.
 *
 * - DOM object: SVGDefsElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#DefsElement
 *
 * ```xml
 * <defs>
 *  <linearGradient id="MyGradient"><!-- ... --></linearGradient>
 * </defs>
 * ```
 */

/**
 * DOM object for a \ref xml_defs element.
 *
 * This element and its children are never rendered directly, but may be referenced by other
 * elements, such as \ref xml_use.
 */
class SVGDefsElement : public SVGGraphicsElement {
private:
  /// Create an SVGDefsElement wrapper from an entity.
  explicit SVGDefsElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Defs;
  /// XML tag name, \ref xml_defs.
  static constexpr std::string_view Tag = "defs";

  /**
   * Create a new \ref xml_defs element.
   *
   * @param document Containing document.
   */
  static SVGDefsElement Create(SVGDocument& document);
};

}  // namespace donner::svg
