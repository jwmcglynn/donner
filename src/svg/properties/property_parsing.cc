#include "src/svg/properties/property_parsing.h"

#include "src/css/parser/value_parser.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/parser/css_transform_parser.h"
#include "src/svg/parser/transform_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

namespace {

template <typename ReturnType, typename FnT>
ReturnType ToConstexpr(ElementType type, FnT fn) {
  switch (type) {
    case ElementType::Circle: return fn(std::integral_constant<ElementType, ElementType::Circle>());
    case ElementType::Defs: return fn(std::integral_constant<ElementType, ElementType::Defs>());
    case ElementType::Path: return fn(std::integral_constant<ElementType, ElementType::Path>());
    case ElementType::Rect: return fn(std::integral_constant<ElementType, ElementType::Rect>());
    case ElementType::Style: return fn(std::integral_constant<ElementType, ElementType::Style>());
    case ElementType::SVG: return fn(std::integral_constant<ElementType, ElementType::SVG>());
    case ElementType::Unknown:
      return fn(std::integral_constant<ElementType, ElementType::Unknown>());
    case ElementType::Use: return fn(std::integral_constant<ElementType, ElementType::Use>());
  };
}

std::span<const css::ComponentValue> TrimTrailingWhitespace(
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
  params.valueOrComponents = TrimTrailingWhitespace(declaration.values);

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

ParseResult<bool> ParseSpecialAttributes(PropertyParseFnParams& params, std::string_view name,
                                         std::optional<ElementType> type, EntityHandle handle) {
  if (StringUtils::EqualsLowercase(name, std::string_view("transform"))) {
    auto& transform = handle.get_or_emplace<TransformComponent>();
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
      std::cerr << "Error parsing " << name << " property: " << maybeError.value() << std::endl;
      return false;
    }
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

}  // namespace donner::svg
