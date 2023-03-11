#pragma once
/// @file

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"

namespace donner::svg {

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
                               AngleParseOptions options = AngleParseOptions::None) {
  if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
    if (dimension->suffix.equalsLowercase("deg")) {
      return dimension->value * MathConstants<double>::kDegToRad;
    } else if (dimension->suffix.equalsLowercase("grad")) {
      return dimension->value * MathConstants<double>::kPi / 200.0;
    } else if (dimension->suffix.equalsLowercase("rad")) {
      return dimension->value;
    } else if (dimension->suffix.equalsLowercase("turn")) {
      return dimension->value * MathConstants<double>::kPi * 2.0;
    } else {
      ParseError err;
      err.reason = "Unsupported angle unit '" + dimension->suffix + "'";
      err.offset = component.sourceOffset();
      return err;
    }
  } else if (const auto* number = component.tryGetToken<css::Token::Number>()) {
    if (options == AngleParseOptions::AllowBareZero && number->valueString == "0") {
      return 0.0;
    }
  }

  ParseError err;
  err.reason = "Invalid angle";
  err.offset = component.sourceOffset();
  return err;
}

}  // namespace donner::svg
