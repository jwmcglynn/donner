#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_desc "<desc>"
 *
 * Provides an extended, human-readable description of its parent element or of the document as a
 * whole. The `<desc>` element and its contents are never rendered as part of the graphic.
 *
 * - DOM object: SVGDescElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#DescElement
 *
 * `<desc>` is a descriptive element. Where \ref xml_title supplies a short accessible name,
 * `<desc>` supplies a longer description that assistive technology can present to users who need
 * more context than the title alone provides. Only the first `<desc>` child of an element is
 * meaningful.
 *
 * ```xml
 * <svg width="100" height="100">
 *   <circle cx="50" cy="50" r="40">
 *     <title>Status indicator</title>
 *     <desc>A solid blue circle indicating the service is online.</desc>
 *   </circle>
 * </svg>
 * ```
 *
 * The text is available programmatically via \ref SVGDescElement::textContent. Because the
 * element is non-rendered, placing a `<desc>` inside a shape (or anywhere else) never changes the
 * rendered pixels.
 */

/**
 * DOM object for a \ref xml_desc element.
 *
 * `<desc>` supplies an extended accessible description for its parent element. It and its children
 * are never rendered; the text content is retained so it can be surfaced through the DOM via \ref
 * textContent.
 */
class SVGDescElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGDescElement wrapper from an entity.
  explicit SVGDescElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGDescElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Desc;
  /// XML tag name, \ref xml_desc.
  static constexpr std::string_view Tag{"desc"};

  /**
   * Create a new \ref xml_desc element.
   *
   * @param document Containing document.
   */
  static SVGDescElement Create(SVGDocument& document) {
    DocumentMutationBatch mutation = CreateElementMutationBatch(document);
    DocumentWriteAccess& access = mutation.access();
    SVGDescElement result = CreateOn(CreateEmptyEntity(access));
    return result;
  }

  /**
   * Return the text content of the `<desc>` element. This is the accessible description for the
   * parent element, and is the authoritative DOM representation of the element's text.
   */
  RcString textContent() const;

  /**
   * Replace the text content of the `<desc>` element with @p text.
   *
   * @param text New description text.
   */
  void setTextContent(std::string_view text);
};

}  // namespace donner::svg
