#pragma once
/// @file

#include "donner/base/xml/XMLQualifiedName.h"
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
   */
  static std::optional<ParseDiagnostic> ApplyParsedAttribute(SVGElement& element,
                                                             const xml::XMLQualifiedNameRef& name,
                                                             std::string_view value);

  /**
   * Remove an XML attribute from the SVG projection without writing back to source.
   *
   * @param element The element to remove the attribute from.
   * @param name The name of the attribute, as specified in the document's XML.
   */
  static void RemoveParsedAttribute(SVGElement& element, const xml::XMLQualifiedNameRef& name);
};

}  // namespace donner::svg::parser
