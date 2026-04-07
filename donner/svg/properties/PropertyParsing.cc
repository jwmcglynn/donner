#include "donner/svg/properties/PropertyParsing.h"

#include "donner/css/parser/ValueParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg::parser {

namespace {

// Presentation attributes have a specificity of 0.
static constexpr css::Specificity kSpecificityPresentationAttribute =
    css::Specificity::FromABC(0, 0, 0);

std::span<const css::ComponentValue> TrimTrailingWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty() && components.back().isToken<css::Token::Whitespace>()) {
    components = components.subspan(0, components.size() - 1);
  }

  return components;
}

}  // namespace

PropertyParseFnParams PropertyParseFnParams::Create(const css::Declaration& declaration,
                                                    css::Specificity specificity,
                                                    PropertyParseBehavior parseBehavior) {
  PropertyParseFnParams params;
  params.valueOrComponents = TrimTrailingWhitespace(declaration.values);
  params.parseBehavior = parseBehavior;

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

PropertyParseFnParams PropertyParseFnParams::CreateForAttribute(std::string_view value) {
  PropertyParseFnParams params;
  params.valueOrComponents = value;
  params.specificity = kSpecificityPresentationAttribute;
  params.parseBehavior = parser::PropertyParseBehavior::AllowUserUnits;

  const std::string_view trimmedValue = StringUtils::TrimWhitespace(value);
  if (StringUtils::EqualsLowercase(trimmedValue, std::string_view("initial"))) {
    params.explicitState = PropertyState::ExplicitInitial;
  } else if (StringUtils::EqualsLowercase(trimmedValue, std::string_view("inherit"))) {
    params.explicitState = PropertyState::Inherit;
  } else if (StringUtils::EqualsLowercase(trimmedValue, std::string_view("unset"))) {
    params.explicitState = PropertyState::ExplicitUnset;
  }

  return params;
}

std::span<const css::ComponentValue> PropertyParseFnParams::components() const {
  if (const std::string_view* str = std::get_if<std::string_view>(&valueOrComponents)) {
    if (!parsedComponents_) {
      parsedComponents_ = std::make_optional(css::parser::ValueParser::Parse(*str));
    }

    return parsedComponents_.value();
  } else {
    return std::get<std::span<const css::ComponentValue>>(valueOrComponents);
  }
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

ParseResult<double> ParseAlphaValue(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      return Clamp(number->value, 0.0, 1.0);
    } else if (const auto* percentage = component.tryGetToken<css::Token::Percentage>()) {
      return Clamp(percentage->value / 100.0, 0.0, 1.0);
    }
  }

  ParseDiagnostic err;
  err.reason = "Invalid alpha value";
  err.range.start = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

}  // namespace donner::svg::parser
