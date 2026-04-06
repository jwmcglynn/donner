#include "donner/svg/components/shape/RectComponent.h"

#include <array>
#include <string_view>

#include "donner/base/CompileTimeMap.h"
#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg::components {
namespace {

using RectPresentationAttributeParseFn = std::optional<ParseDiagnostic> (*)(
    RectProperties& properties, const parser::PropertyParseFnParams& params);

constexpr std::array<std::pair<std::string_view, RectPresentationAttributeParseFn>, 6>
    kPropertyEntries{{
        {"x",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.x);
         }},
        {"y",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.y);
         }},
        {"width",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.width);
         }},
        {"height",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.height);
         }},
        {"rx",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.rx);
         }},
        {"ry",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.ry);
         }},
    }};

constexpr auto kProperties = makeCompileTimeMap(kPropertyEntries);

}  // namespace

ComputedRectComponent::ComputedRectComponent(
    const RectProperties& inputProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    std::vector<ParseDiagnostic>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const RectPresentationAttributeParseFn* parseFn =
        kProperties.find(static_cast<std::string_view>(name));
    if (parseFn != nullptr) {
      auto maybeError = (*parseFn)(properties, parser::PropertyParseFnParams::Create(
                                                   property.declaration, property.specificity,
                                                   parser::PropertyParseBehavior::AllowUserUnits));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

ParseResult<bool> ParseRectPresentationAttribute(EntityHandle handle, std::string_view name,
                                                  const parser::PropertyParseFnParams& params) {
  const RectPresentationAttributeParseFn* parseFn = kProperties.find(name);
  if (parseFn != nullptr) {
    RectProperties& properties = handle.get_or_emplace<RectComponent>().properties;
    auto maybeError = (*parseFn)(properties, params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully.
      return true;
    }
  }

  return false;
}

}  // namespace donner::svg::components
