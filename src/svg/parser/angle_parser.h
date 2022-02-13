#pragma once

#include <span>

#include "src/base/parser/parse_result.h"
#include "src/css/declaration.h"

namespace donner::svg {

enum class AngleParseOptions { None, AllowBareZero };

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
