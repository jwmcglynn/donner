#include "donner/svg/components/shape/CircleComponent.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

using CirclePresentationAttributeParseFn = std::optional<ParseError> (*)(
    CircleProperties& properties, const parser::PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, CirclePresentationAttributeParseFn, 3>
    kProperties = {
        {"cx",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cx);
         }},  //
        {"cy",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cy);
         }},  //
        {"r",
         [](CircleProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.r);
         }}  //

};

}  // namespace

ComputedCircleComponent::ComputedCircleComponent(
    const CircleProperties& inputProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    std::vector<ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError = it->second(properties, parser::PropertyParseFnParams::Create(
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
  const auto it = components::kProperties.find(frozen::string(name));
  if (it != components::kProperties.end()) {
    components::CircleProperties& properties =
        handle.get_or_emplace<components::CircleComponent>().properties;
    auto maybeError = it->second(properties, params);
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
