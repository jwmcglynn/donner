#include "donner/svg/components/shape/CircleComponent.h"

#include <array>
#include <string_view>

#include "donner/base/CompileTimeMap.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

using CirclePresentationAttributeParseFn = std::optional<ParseError> (*)(
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

constexpr auto kPropertiesResult = makeCompileTimeMap(kPropertyEntries);
static_assert(kPropertiesResult.status == CompileTimeMapStatus::kOk);
constexpr auto kProperties = kPropertiesResult.map;

}  // namespace

ComputedCircleComponent::ComputedCircleComponent(
    const CircleProperties& inputProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    std::vector<ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const CirclePresentationAttributeParseFn* parseFn =
        kProperties.find(static_cast<std::string_view>(name));
    if (parseFn != nullptr) {
      auto maybeError =
          (*parseFn)(properties, parser::PropertyParseFnParams::Create(
                         property.declaration, property.specificity,
                         parser::PropertyParseBehavior::AllowUserUnits));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Circle>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  const components::CirclePresentationAttributeParseFn* parseFn =
      components::kProperties.find(name);
  if (parseFn != nullptr) {
    components::CircleProperties& properties =
        handle.get_or_emplace<components::CircleComponent>().properties;
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

}  // namespace donner::svg::parser
