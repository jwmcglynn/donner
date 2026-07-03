#pragma once
/// @file

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

/**
 * @page xml_switch "<switch>"
 *
 * Conditional processing container: renders only the first direct child whose
 * conditional-processing attributes all evaluate to true.
 *
 * - DOM object: SVGSwitchElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#SwitchElement
 *
 * The `<switch>` element evaluates the conditional-processing attributes
 * (`requiredExtensions` and `systemLanguage`) on each of its direct children in document order,
 * and renders the first child for which all of them evaluate to true. Children without
 * conditional attributes evaluate to true, so a final unconditional child acts as a fallback.
 * Unknown (non-SVG) child elements are never selected.
 *
 * Aside from child selection, `<switch>` behaves like \ref xml_g: attributes such as `transform`
 * and `fill` set on it apply to the rendered child.
 *
 * ```xml
 * <switch>
 *   <rect width="100" height="100" fill="red" requiredExtensions="http://example.org/bogus" />
 *   <rect width="100" height="100" fill="green" />
 * </switch>
 * ```
 *
 * \htmlonly
 * <svg id="xml_switch" width="300" height="300" style="background-color: white">
 * <switch>
 *   <rect width="100" height="100" fill="red" requiredExtensions="http://example.org/bogus" />
 *   <rect width="100" height="100" fill="green" />
 * </switch>
 * </svg>
 * \endhtmlonly
 */

/**
 * DOM object for a \ref xml_switch element.
 */
class SVGSwitchElement : public SVGGraphicsElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGSwitchElement wrapper from an entity.
  explicit SVGSwitchElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGSwitchElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Switch;
  /// XML tag name, \ref xml_switch.
  static constexpr std::string_view Tag{"switch"};

  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_switch element.
   *
   * @param document Containing document.
   */
  static SVGSwitchElement Create(SVGDocument& document) {
    DocumentMutationBatch mutation = CreateElementMutationBatch(document);
    DocumentWriteAccess& access = mutation.access();
    SVGSwitchElement result = CreateOn(CreateEmptyEntity(access));
    return result;
  }
};

}  // namespace donner::svg
