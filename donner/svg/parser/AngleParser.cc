#include "donner/svg/parser/AngleParser.h"

namespace donner::svg::parser {

ParseResult<double> ParseAngle(const css::ComponentValue& component, AngleParseOptions options) {
  if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
    if (dimension->suffixString.equalsLowercase("deg")) {
      return dimension->value * MathConstants<double>::kDegToRad;
    } else if (dimension->suffixString.equalsLowercase("grad")) {
      return dimension->value * MathConstants<double>::kPi / 200.0;
    } else if (dimension->suffixString.equalsLowercase("rad")) {
      return dimension->value;
    } else if (dimension->suffixString.equalsLowercase("turn")) {
      return dimension->value * MathConstants<double>::kPi * 2.0;
    } else {
      ParseError err;
      err.reason = "Unsupported angle unit '" + dimension->suffixString + "'";
      err.location = component.sourceOffset();
      return err;
    }
  } else if (const auto* number = component.tryGetToken<css::Token::Number>()) {
    if (options == AngleParseOptions::AllowBareZero && number->valueString == "0") {
      return 0.0;
    }
  }

  ParseError err;
  err.reason = "Invalid angle";
  err.location = component.sourceOffset();
  return err;
}

}  // namespace donner::svg::parser
