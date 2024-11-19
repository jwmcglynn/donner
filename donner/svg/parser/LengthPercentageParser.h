#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/base/ParseResult.h"
#include "donner/css/ComponentValue.h"

namespace donner::svg::parser {

/**
 * Parse a `<length-percentage>` value.
 *
 * @param component A single component value.
 * @param allowUserUnits Whether to allow unitless values, if this is a parse in the context of XML
 *   attributes.
 * @return Return a Length or a parse error.
 */
ParseResult<Lengthd> ParseLengthPercentage(const css::ComponentValue& component,
                                           bool allowUserUnits);

/**
 * Parse a `<length-percentage>` value.
 *
 * @param components Component values, which should already be trimmed.
 * @param allowUserUnits Whether to allow unitless values, if this is a parse in the context of XML
 *   attributes.
 * @return Return a Length or a parse error.
 */
ParseResult<Lengthd> ParseLengthPercentage(std::span<const css::ComponentValue> components,
                                           bool allowUserUnits);

}  // namespace donner::svg::parser
