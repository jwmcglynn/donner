#pragma once
/// @file

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/parser/details/SVGParserContext.h"

namespace donner::svg::parser {

/**
 * Parses XML attributes and sets applies them to SVGElements.
 */
class AttributeParser {
public:
  /**
   * Parse an XML attribute and set it on the given \c element.
   *
   * @param context The parser context, used to store XML document metadata and store warnings.
   * @param element The element to set the attribute on.
   * @param name The name of the attribute, as specified in the document's XML.
   * @param value The value of the attribute.
   */
  static std::optional<ParseDiagnostic> ParseAndSetAttribute(SVGParserContext& context,
                                                             SVGElement& element,
                                                             const xml::XMLQualifiedNameRef& name,
                                                             std::string_view value) noexcept;

  /**
   * Apply an XML attribute read by the SVG parser without writing back to source.
   *
   * @param element The element to set the attribute on.
   * @param name The name of the attribute, as specified in the document's XML.
   * @param value The value of the attribute.
   * @param[out] consumedAsPresentationAttribute When non-null, set to true if the value was
   *   recognized and applied as a presentation attribute, and false if it was stored only as a
   *   generic XML attribute.
   */
  static std::optional<ParseDiagnostic> ApplyParsedAttribute(
      SVGElement& element, const xml::XMLQualifiedNameRef& name, std::string_view value,
      bool* consumedAsPresentationAttribute = nullptr);

  /**
   * Remove an XML attribute from the SVG projection without writing back to source.
   *
   * @param element The element to remove the attribute from.
   * @param name The name of the attribute, as specified in the document's XML.
   */
  static void RemoveParsedAttribute(SVGElement& element, const xml::XMLQualifiedNameRef& name);

  /**
   * After \p editedName was set on or removed from a filter element (see
   * \ref IsFilterElementFamily), reset the element's typed filter state to its defaults and parse
   * its stored type-specific attributes again, as a fresh parse of the element would. Presentation
   * and generic attributes are not touched, and nothing happens when \p editedName is not one of
   * the element's type-specific attributes.
   *
   * @param context Parser context; its warning sink receives the reparsed attributes' warnings.
   * @param element The edited element.
   * @param editedName The attribute that was set or removed.
   */
  static components::TypedFilterAttributeEdit ReapplyTypedFilterAttributes(
      SVGParserContext& context, SVGElement& element, const xml::XMLQualifiedNameRef& editedName);
};

}  // namespace donner::svg::parser
