#pragma once
/// @file

#include "src/svg/properties/property_parsing.h"
#include "src/svg/registry/registry.h"  // For EntityHandle

namespace donner::svg {

/**
 * Parse a presentation attribute,which can contain a CSS value, for a specific \ref ElementType.
 *
 * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
 *
 * @tparam Type Type of element to parse presentation attributes for.
 * @param name Name of the attribute.
 * @param params Parameters for parsing the attribute, which includes the value to parse,
 *   specificity, and parser options.
 * @return true if the element supports this attribute and it was parsed successfully, or a \ref
 *   ParseError if parsing failed.
 */
template <ElementType Type>
ParseResult<bool> ParsePresentationAttribute(EntityHandle handle, std::string_view name,
                                             const PropertyParseFnParams& params);

}  // namespace donner::svg
