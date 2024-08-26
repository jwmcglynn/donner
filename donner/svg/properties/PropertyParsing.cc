#include "donner/svg/properties/PropertyParsing.h"

#include "donner/css/parser/ValueParser.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/parser/CssTransformParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/parser/TransformParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"

namespace donner::svg::parser {

namespace {

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

ParseResult<bool> ParseSpecialAttributes(PropertyParseFnParams& params, std::string_view name,
                                         std::optional<ElementType> type, EntityHandle handle) {
  if (StringUtils::EqualsLowercase(name, std::string_view("transform"))) {
    auto& transform = handle.get_or_emplace<components::TransformComponent>();
    auto maybeError = Parse(
        params,
        [](const PropertyParseFnParams& params) {
          if (const std::string_view* str =
                  std::get_if<std::string_view>(&params.valueOrComponents)) {
            return TransformParser::Parse(*str).map<CssTransform>(
                [](const Transformd& transform) { return CssTransform(transform); });
          } else {
            return CssTransformParser::Parse(params.components());
          }
        },
        &transform.transform);
    if (maybeError) {
      return std::move(maybeError.value());
    }

    return true;
  }

  if (!type.has_value()) {
    // Stop processing if there is not an element type.
    return false;
  }

  return ToConstexpr<ParseResult<bool>>(type.value(), [&](auto elementType) {
    return ParsePresentationAttribute<elementType()>(handle, name, params);
  });
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

  ParseError err;
  err.reason = "Invalid alpha value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

}  // namespace donner::svg::parser
