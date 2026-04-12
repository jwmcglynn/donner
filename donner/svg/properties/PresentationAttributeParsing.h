#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry_fwd.h"  // For EntityHandle
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::parser {

/**
 * Parse a presentation attribute, which can contain a CSS value, for a specific \ref ElementType.
 *
 * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
 *
 * @param type Type of element to parse presentation attributes for.
 * @param handle Entity handle which determines which attributes are supported, and where to save
 * the parsed value.
 * @param name Name of the attribute.
 * @param params Parameters for parsing the attribute, which includes the value to parse,
 * specificity, and parser options.
 * @return true if the element supports this attribute and it was parsed successfully, or a \ref
 * ParseDiagnostic if parsing failed.
 */
ParseResult<bool> ParsePresentationAttribute(ElementType type, EntityHandle handle,
                                             std::string_view name,
                                             const PropertyParseFnParams& params);

}  // namespace donner::svg::parser
