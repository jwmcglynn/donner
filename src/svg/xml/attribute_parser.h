#pragma once
/// @file

#include "src/svg/xml/details/xml_parser_context.h"
#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg::parser {

/**
 * Parses XML attributes and sets applies them to SVGElements.
 */
class AttributeParser {
public:
  /**
   * Parse an XML attribute and set it on the given \ref element.
   *
   * @param context The parser context, used to store XML document metadata and store warnings.
   * @param element The element to set the attribute on.
   * @param name The name of the attribute, as specified in the document's XML.
   * @param value The value of the attribute.
   */
  static std::optional<ParseError> ParseAndSetAttribute(XMLParserContext& context,
                                                        SVGElement& element,
                                                        const XMLQualifiedNameRef& name,
                                                        std::string_view value) noexcept;
};

}  // namespace donner::svg::parser
