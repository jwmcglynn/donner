#include "src/svg/parser/length_percentage_parser.h"

namespace donner::svg {

ParseResult<Lengthd> ParseLengthPercentage(const css::ComponentValue& component,
                                           bool allowUserUnits) {
  if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
    if (!dimension->suffixUnit) {
      ParseError err;
      err.reason = "Invalid unit on length";
      err.offset = component.sourceOffset();
      return err;
    } else {
      return Lengthd(dimension->value, dimension->suffixUnit.value());
    }
  } else if (const auto* percentage = component.tryGetToken<css::Token::Percentage>()) {
    return Lengthd(percentage->value, Lengthd::Unit::Percent);
  } else if (const auto* number = component.tryGetToken<css::Token::Number>()) {
    if (!allowUserUnits && number->valueString == "0") {
      return Lengthd(0, Lengthd::Unit::None);
    } else if (allowUserUnits) {
      return Lengthd(number->value, Lengthd::Unit::None);
    }
  }

  ParseError err;
  err.reason = "Invalid length or percentage";
  err.offset = component.sourceOffset();
  return err;
}

ParseResult<Lengthd> ParseLengthPercentage(std::span<const css::ComponentValue> components,
                                           bool allowUserUnits) {
  if (components.size() == 1) {
    return ParseLengthPercentage(components.front(), allowUserUnits);
  } else if (components.empty()) {
    ParseError err;
    err.reason = "Unexpected end of input";
    err.offset = ParseError::kEndOfString;
    return err;
  }

  ParseError err;
  err.reason = "Unexpected token when parsing length or percentage";
  err.offset = components[1].sourceOffset();
  return err;
}

}  // namespace donner::svg
