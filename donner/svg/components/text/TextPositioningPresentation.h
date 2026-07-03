#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseResult.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

/**
 * Parse the text positioning presentation attributes (`x`, `y`, `dx`, `dy`, `rotate`) on
 * `<text>` / `<tspan>` / `<textPath>` elements into \ref TextPositioningComponent, and
 * invalidate the text root's cached layout so the next render picks the new positions up.
 *
 * This is the programmatic-mutation counterpart of the XML parse path
 * (`ParseAttribute<SVGTextElement>` in `AttributeParser.cc`): without it, a `setAttribute("x", …)`
 * on a text element stores the raw attribute but never updates the layout input, so
 * programmatically positioned text renders at the origin.
 *
 * @param handle Entity handle of the text content element.
 * @param name Attribute name.
 * @param params Parse parameters containing the attribute value.
 * @return true if the attribute was handled, false for unrelated attributes.
 */
ParseResult<bool> ParseTextPositioningPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params);

}  // namespace donner::svg::components
