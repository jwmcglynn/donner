#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_title "<title>"
 *
 * Provides a human-readable, accessible name for its parent element or for the document as a
 * whole. The `<title>` element and its contents are never rendered as part of the graphic.
 *
 * - DOM object: SVGTitleElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#TitleElement
 *
 * `<title>` is a descriptive element: it holds plain text that describes what its parent element
 * represents. User agents typically expose this text as an accessible name (for screen readers)
 * and may present it as a tooltip on hover. Only the first `<title>` child of an element is
 * meaningful, and it should be the first child so assistive technology encounters it early.
 *
 * ```xml
 * <svg width="100" height="100">
 *   <circle cx="50" cy="50" r="40">
 *     <title>A blue circle</title>
 *   </circle>
 * </svg>
 * ```
 *
 * The text is available programmatically via \ref SVGTitleElement::textContent. Because the
 * element is non-rendered, placing a `<title>` inside a shape (or anywhere else) never changes
 * the rendered pixels.
 */

/**
 * DOM object for a \ref xml_title element.
 *
 * `<title>` supplies an accessible name for its parent element. It and its children are never
 * rendered; the text content is retained so it can be surfaced through the DOM via \ref
 * textContent.
 */
class SVGTitleElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGTitleElement wrapper from an entity.
  explicit SVGTitleElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGTitleElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Title;
  /// XML tag name, \ref xml_title.
  static constexpr std::string_view Tag{"title"};

  /**
   * Create a new \ref xml_title element.
   *
   * @param document Containing document.
   */
  static SVGTitleElement Create(SVGDocument& document) {
    DocumentMutationBatch mutation = CreateElementMutationBatch(document);
    DocumentWriteAccess& access = mutation.access();
    SVGTitleElement result = CreateOn(CreateEmptyEntity(access));
    return result;
  }

  /**
   * Return the text content of the `<title>` element. This is the accessible name for the parent
   * element, and is the authoritative DOM representation of the element's text.
   */
  RcString textContent() const;

  /**
   * Replace the text content of the `<title>` element with @p text.
   *
   * @param text New title text.
   */
  void setTextContent(std::string_view text);
};

}  // namespace donner::svg
