#include "src/svg/components/shape/ellipse_component.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/svg/parser/length_percentage_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

namespace {

using EllipsePresentationAttributeParseFn = std::optional<parser::ParseError> (*)(
    EllipseProperties& properties, const parser::PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, EllipsePresentationAttributeParseFn, 4>
    kProperties = {
        {"cx",
         [](EllipseProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cx);
         }},  //
        {"cy",
         [](EllipseProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
               },
               &properties.cy);
         }},  //
        {"rx",
         [](EllipseProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.rx);
         }},  //
        {"ry",
         [](EllipseProperties& properties, const parser::PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const parser::PropertyParseFnParams& params) {
                 return parser::ParseLengthPercentageOrAuto(params.components(),
                                                            params.allowUserUnits());
               },
               &properties.ry);
         }}  //

};

}  // namespace

ComputedEllipseComponent::ComputedEllipseComponent(
    const EllipseProperties& inputProperties,
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
ParseResult<bool> ParsePresentationAttribute<ElementType::Ellipse>(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  const auto it = components::kProperties.find(frozen::string(name));
  if (it != components::kProperties.end()) {
    components::EllipseProperties& properties =
        handle.get_or_emplace<components::EllipseComponent>().properties;
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
