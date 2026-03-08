#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

#include "donner/css/parser/ColorParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeFlood>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  auto& comp = handle.get_or_emplace<components::FEFloodComponent>();
  if (name == "flood-color") {
    if (auto maybeError = Parse(
            params,
            [](const PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.floodColor)) {
      return std::move(maybeError.value());
    }
    return true;
  } else if (name == "flood-opacity") {
    if (auto maybeError = Parse(
            params,
            [](const PropertyParseFnParams& params) {
              return ParseAlphaValue(params.components());
            },
            &comp.floodOpacity)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::FeDropShadow>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  auto& comp = handle.get_or_emplace<components::FEDropShadowComponent>();
  if (name == "flood-color") {
    if (auto maybeError = Parse(
            params,
            [](const PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.floodColor)) {
      return std::move(maybeError.value());
    }
    return true;
  } else if (name == "flood-opacity") {
    if (auto maybeError = Parse(
            params,
            [](const PropertyParseFnParams& params) {
              return ParseAlphaValue(params.components());
            },
            &comp.floodOpacity)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

}  // namespace donner::svg::parser
