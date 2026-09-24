#pragma once
/// @file

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

/**
 * @page xml_switch &lt;switch&gt;
 *
 * Conditional processing container: renders the first matching direct child, ranking any
 * language-conditioned children before it by the user's preferred languages.
 *
 * - DOM object: SVGSwitchElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#SwitchElement
 *
 * The `<switch>` element ranks its direct children carrying `systemLanguage` by the priority
 * order configured with `SVGDocument::setUserLanguages` (matching SVG2's allowReorder=yes
 * behavior) and renders the best match, tie-breaking by document order. A child without
 * `systemLanguage` matches at its document position; later children cannot displace it. Authors
 * can place an unconditional child last to use it as a fallback. The SVG 2 rule applies whether
 * or not the historical `allowReorder` attribute is present. Unknown (non-SVG) child elements
 * are never selected.
 *
 * Donner evaluates `systemLanguage` against the user's preferred languages (default `en`, see
 * `SVGDocument::setUserLanguages`), treats a non-empty `requiredExtensions` list as
 * unsupported, and ignores `requiredFeatures` because SVG2 deprecates it.
 *
 * Aside from child selection, `<switch>` behaves like \ref xml_g; attributes such as `transform`
 * and `fill` set on it apply to the rendered child.
 *
 * ## Attributes
 *
 * `<switch>` has no element-specific attributes of its own. It uses standard SVG presentation
 * attributes plus the common conditional-processing attributes `requiredFeatures`,
 * `requiredExtensions`, and `systemLanguage`.
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
