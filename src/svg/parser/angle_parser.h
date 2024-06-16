#pragma once
/// @file

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"

namespace donner::svg::parser {

/**
 * Options for \ref ParseAngle, which controls whether bare zero is allowed.
 */
enum class AngleParseOptions {
  None,          ///< Angles require a dimension suffix, such as '30deg' or '2rad'.
  AllowBareZero  ///< Allow '0' to be parsed as an angle.
};

/**
 * Parse an angle value within a CSS property, such as '30deg' or '2rad'.
 *
 * @param component The CSS component value to parse.
 * @param options Options for parsing.
 * @return The angle in radians, or a \ref ParseError if parsing failed.
 */
ParseResult<double> ParseAngle(const css::ComponentValue& component,
                               AngleParseOptions options = AngleParseOptions::None);

}  // namespace donner::svg::parser
