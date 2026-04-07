#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg::parser {

ParseResult<Lengthd> ParseLengthPercentage(const css::ComponentValue& component,
                                           bool allowUserUnits) {
  if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
    if (!dimension->suffixUnit) {
      ParseDiagnostic err;
      err.reason = "Invalid unit on length";
      err.range = {component.sourceOffset(), component.sourceOffset()};
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

  ParseDiagnostic err;
  err.reason = "Invalid length or percentage";
  err.range = {component.sourceOffset(), component.sourceOffset()};
  return err;
}

ParseResult<Lengthd> ParseLengthPercentage(std::span<const css::ComponentValue> components,
                                           bool allowUserUnits) {
  if (components.size() == 1) {
    return ParseLengthPercentage(components.front(), allowUserUnits);
  } else if (components.empty()) {
    ParseDiagnostic err;
    err.reason = "Unexpected end of input";
    err.range = {FileOffset::EndOfString(), FileOffset::EndOfString()};
    return err;
  }

  ParseDiagnostic err;
  err.reason = "Unexpected token when parsing length or percentage";
  err.range = {components[1].sourceOffset(), components[1].sourceOffset()};
  return err;
}

}  // namespace donner::svg::parser
