#pragma once
/// @file

#include <optional>

#include "donner/base/RcString.h"
#include "donner/svg/SVGTextPositioningElement.h"

namespace donner::svg {

/**
 * @page xml_a "<a>"
 *
 * The `<a>` element creates a hyperlink around its child content. It is a transparent grouping
 * container: it draws nothing of its own and does not establish a new coordinate system, but its
 * children render in-place exactly as if the `<a>` were not there.
 *
 * - DOM object: SVGAElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/linking.html#AElement
 *
 * Unlike \ref xml_g, `<a>` is allowed both inside and outside of text content. When it appears
 * inside a \ref xml_text (or \ref xml_tspan) flow it behaves like a \ref xml_tspan — a text-content
 * group whose text children participate in the surrounding text layout, and which supports the
 * per-glyph positioning attributes `x`, `y`, `dx`, `dy`, and `rotate`. When it appears outside of
 * text it behaves like a \ref xml_g, grouping arbitrary graphics elements.
 *
 * The link target is given by `href` (or the legacy `xlink:href`). Donner has no interactive
 * navigation, so the target is parsed and retained but does not affect rendering.
 *
 * ```svg
 * <text x="20" y="40">
 *   <a href="https://www.w3.org/TR/SVG2/">SVG 2</a>
 * </text>
 * ```
 */

/**
 * DOM object for a \ref xml_a element.
 *
 * `<a>` is a transparent grouping element that wraps its children in a hyperlink. It acts as a
 * text-content group (like \ref xml_tspan) when nested inside text, and as a general grouping
 * container (like \ref xml_g) otherwise.
 *
 * \see https://www.w3.org/TR/SVG2/linking.html#AElement
 */
class SVGAElement : public SVGTextPositioningElement {
  friend class parser::SVGParserImpl;

private:
  /// Create an SVGAElement wrapper from an entity.
  explicit SVGAElement(EntityHandle handle) : SVGTextPositioningElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGAElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::A;
  /// XML tag name, \ref xml_a.
  static constexpr std::string_view Tag{"a"};

  static_assert(SVGTextPositioningElement::IsBaseOf(Type));
  static_assert(SVGTextContentElement::IsBaseOf(Type));
  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new \ref xml_a element.
   *
   * @param document Containing document.
   */
  static SVGAElement Create(SVGDocument& document) {
    DocumentMutationBatch mutation = CreateElementMutationBatch(document);
    DocumentWriteAccess& access = mutation.access();
    SVGAElement result = CreateOn(CreateEmptyEntity(access));
    return result;
  }

  /**
   * Set the hyperlink target (`href` / `xlink:href`).
   *
   * @param value Link target, or \c std::nullopt to clear it.
   */
  void setHref(const std::optional<RcString>& value);

  /**
   * Get the hyperlink target (`href` / `xlink:href`).
   *
   * @return Link target, or \c std::nullopt if not set.
   */
  std::optional<RcString> href() const;
};

}  // namespace donner::svg
