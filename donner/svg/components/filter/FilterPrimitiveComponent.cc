#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

#include "donner/base/EcsRegistry.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

ParseResult<bool> ParseFeFloodPresentationAttribute(EntityHandle handle, std::string_view name,
                                                    const parser::PropertyParseFnParams& params) {
  auto& comp = handle.get_or_emplace<FEFloodComponent>();
  if (name == "flood-color") {
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.floodColor)) {
      return std::move(maybeError.value());
    }
    return true;
  } else if (name == "flood-opacity") {
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return parser::ParseAlphaValue(params.components());
            },
            &comp.floodOpacity)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

ParseResult<bool> ParseFeDropShadowPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  auto& comp = handle.get_or_emplace<FEDropShadowComponent>();
  if (name == "flood-color") {
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.floodColor)) {
      return std::move(maybeError.value());
    }
    return true;
  } else if (name == "flood-opacity") {
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return parser::ParseAlphaValue(params.components());
            },
            &comp.floodOpacity)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

ParseResult<bool> ParseFeDiffuseLightingPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  if (name == "lighting-color") {
    auto& comp = handle.get_or_emplace<FEDiffuseLightingComponent>();
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.lightingColor)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

ParseResult<bool> ParseFeSpecularLightingPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  if (name == "lighting-color") {
    auto& comp = handle.get_or_emplace<FESpecularLightingComponent>();
    if (auto maybeError = Parse(
            params,
            [](const parser::PropertyParseFnParams& params) {
              return css::parser::ColorParser::Parse(params.components());
            },
            &comp.lightingColor)) {
      return std::move(maybeError.value());
    }
    return true;
  }
  return false;
}

}  // namespace donner::svg::components
