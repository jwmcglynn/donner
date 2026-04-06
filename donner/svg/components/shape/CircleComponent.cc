#include "donner/svg/components/shape/CircleComponent.h"

#include <array>
#include <string_view>

#include "donner/base/CompileTimeMap.h"
#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg::components {

namespace {

using CirclePresentationAttributeParseFn = std::optional<ParseDiagnostic> (*)(
    CircleProperties& properties, const parser::PropertyParseFnParams& params);

constexpr std::array<std::pair<std::string_view, CirclePresentationAttributeParseFn>, 3>
    kPropertyEntries{{
        {"cx",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cx);
         }},
        {"cy",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cy);
         }},
        {"r",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.r);
         }},
    }};

constexpr auto kProperties = makeCompileTimeMap(kPropertyEntries);

}  // namespace

ComputedCircleComponent::ComputedCircleComponent(
    const CircleProperties& inputProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    ParseWarningSink& warningSink)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const CirclePresentationAttributeParseFn* parseFn =
        kProperties.find(static_cast<std::string_view>(name));
    if (parseFn != nullptr) {
      auto maybeError =
          (*parseFn)(properties, parser::PropertyParseFnParams::Create(
                         property.declaration, property.specificity,
                         parser::PropertyParseBehavior::AllowUserUnits));
      if (maybeError) {
        warningSink.add(std::move(maybeError.value()));
      }
    }
  }
}

ParseResult<bool> ParseCirclePresentationAttribute(EntityHandle handle, std::string_view name,
                                                    const parser::PropertyParseFnParams& params) {
  const CirclePresentationAttributeParseFn* parseFn = kProperties.find(name);
  if (parseFn != nullptr) {
    CircleProperties& properties = handle.get_or_emplace<CircleComponent>().properties;
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
