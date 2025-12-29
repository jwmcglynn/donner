#pragma once
/// @file

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/base/Length.h"
#include "donner/svg/core/Overflow.h"
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
  static std::optional<ParseError> ParseAndSetAttribute(SVGParserContext& context,
                                                        SVGElement& element,
                                                        const xml::XMLQualifiedNameRef& name,
                                                        std::string_view value) noexcept;
};

std::optional<Lengthd> ParseLengthAttribute(SVGParserContext& context, std::string_view value);

std::optional<Overflow> ParseFlowOverflow(std::string_view value);

}  // namespace donner::svg::parser
