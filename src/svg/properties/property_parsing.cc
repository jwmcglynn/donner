#include "src/svg/properties/property_parsing.h"

#include "src/css/parser/value_parser.h"

namespace donner::svg {

namespace {

std::span<const css::ComponentValue> trimTrailingWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty() && components.back().isToken<css::Token::Whitespace>()) {
    components = components.subspan(0, components.size() - 1);
  }

  return components;
}

}  // namespace

std::span<const css::ComponentValue> PropertyParseFnParams::components() const {
  if (const std::string_view* str = std::get_if<std::string_view>(&valueOrComponents)) {
    if (!parsedComponents_) {
      parsedComponents_ = std::make_optional(css::ValueParser::Parse(*str));
    }

    return parsedComponents_.value();
  } else {
    return std::get<std::span<const css::ComponentValue>>(valueOrComponents);
  }
}

PropertyParseFnParams CreateParseFnParams(const css::Declaration& declaration,
                                          css::Specificity specificity) {
  PropertyParseFnParams params;
  params.valueOrComponents = trimTrailingWhitespace(declaration.values);

  // Detect CSS-wide keywords, see https://www.w3.org/TR/css-cascade-3/#defaulting-keywords.
  const auto components = params.components();
  if (components.size() == 1 && components.front().isToken<css::Token::Ident>()) {
    const RcString& ident = components.front().get<css::Token>().get<css::Token::Ident>().value;
    if (ident.equalsLowercase("initial")) {
      params.explicitState = PropertyState::ExplicitInitial;
    } else if (ident.equalsLowercase("inherit")) {
      params.explicitState = PropertyState::Inherit;
    } else if (ident.equalsLowercase("unset")) {
      params.explicitState = PropertyState::ExplicitUnset;
    }
  }

  params.specificity = declaration.important ? css::Specificity::Important() : specificity;

  return params;
}

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
