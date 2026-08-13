#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @page xml_metadata &lt;metadata&gt;
 *
 * Container for metadata about the document or its parent element, such as authorship, licensing,
 * or RDF descriptions. The `<metadata>` element and its contents are never rendered.
 *
 * - DOM object: SVGMetadataElement
 * - SVG2 spec: https://www.w3.org/TR/SVG2/struct.html#MetadataElement
 *
 * `<metadata>` is a descriptive element. SVG metadata is often structured data from another XML
 * namespace (for example, Dublin Core or RDF), and is meaningful only to processors that
 * understand that data, not to the SVG renderer. Donner recognizes the element so it is not
 * treated as an unknown element and keeps it out of the render tree. The convenience
 * \ref SVGMetadataElement::textContent accessor retains only direct text and CDATA children; it
 * does not concatenate text nested inside child elements.
 *
 * ```xml
 * <svg width="100" height="100">
 *   <metadata>
 *     Author: Jane Doe
 *   </metadata>
 *   <circle cx="50" cy="50" r="40" />
 * </svg>
 * ```
 *
 * The concatenated direct text and CDATA content is available programmatically via \ref
 * SVGMetadataElement::textContent. Because the element is non-rendered, `<metadata>` never changes
 * the rendered pixels.
 */

/**
 * DOM object for a \ref xml_metadata element.
 *
 * `<metadata>` holds metadata about the document or its parent element. It and its children are
 * never rendered; its direct text and CDATA content is retained so it can be surfaced through the
 * DOM via \ref textContent.
 */
class SVGMetadataElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGMetadataElement wrapper from an entity.
  explicit SVGMetadataElement(EntityHandle handle) : SVGElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGMetadataElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Metadata;
  /// XML tag name, \ref xml_metadata.
  static constexpr std::string_view Tag{"metadata"};

  /**
   * Create a new \ref xml_metadata element.
   *
   * @param document Containing document.
   */
  static SVGMetadataElement Create(SVGDocument& document) {
    DocumentMutationBatch mutation = CreateElementMutationBatch(document);
    DocumentWriteAccess& access = mutation.access();
    SVGMetadataElement result = CreateOn(CreateEmptyEntity(access));
    return result;
  }

  /**
   * Return the concatenated direct text and CDATA children of the `<metadata>` element.
   *
   * Text nested inside child elements is not included.
   */
  RcString textContent() const;

  /**
   * Replace the text content of the `<metadata>` element with @p text.
   *
   * @param text New metadata text.
   */
  void setTextContent(std::string_view text);
};

}  // namespace donner::svg
