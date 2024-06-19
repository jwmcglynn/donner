#include "donner/svg/components/shape/RectComponent.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {
namespace {

using RectPresentationAttributeParseFn = std::optional<parser::ParseError> (*)(
    RectProperties& properties, const parser::PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, RectPresentationAttributeParseFn, 6>
    kProperties = {
        {"x",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.x);
         }},  //
        {"y",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.y);
         }},  //
        {"width",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.width);
         }},  //
        {"height",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.height);
         }},  //
        {"rx",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.rx);
         }},  //
        {"ry",
         [](RectProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.ry);
         }},  //
};

}  // namespace

ComputedRectComponent::ComputedRectComponent(
    const RectProperties& inputProperties,
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    std::vector<parser::ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError = it->second(
          properties, CreateParseFnParams(property.declaration, property.specificity,
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
ParseResult<bool> ParsePresentationAttribute<ElementType::Rect>(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  const auto it = components::kProperties.find(frozen::string(name));
  if (it != components::kProperties.end()) {
    components::RectProperties& properties =
        handle.get_or_emplace<components::RectComponent>().properties;
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
