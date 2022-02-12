#include "src/svg/properties/property_parsing.h"

namespace donner::svg {

std::optional<RcString> TryGetSingleIdent(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      return ident->value;
    }
  }

  return std::nullopt;
}

ParseResult<Lengthd> ParseLengthPercentage(std::span<const css::ComponentValue> components,
                                           bool allowUserUnits) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
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
  }

  ParseError err;
  err.reason = "Invalid length or percentage";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

ParseResult<std::optional<Lengthd>> ParseLengthPercentageOrAuto(
    std::span<const css::ComponentValue> components, bool allowUserUnits) {
  using ReturnType = std::optional<Lengthd>;

  if (auto maybeIdent = TryGetSingleIdent(components);
      maybeIdent && maybeIdent->equalsLowercase("auto")) {
    return ReturnType(std::nullopt);
  } else {
    return ParseLengthPercentage(components, allowUserUnits).map<ReturnType>([](Lengthd value) {
      return value;
    });
  }
}

}  // namespace donner::svg
