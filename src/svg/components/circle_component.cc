#include "src/svg/components/circle_component.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

namespace {

using CirclePresentationAttributeParseFn = std::optional<ParseError> (*)(
    CircleProperties& properties, const PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, CirclePresentationAttributeParseFn, 3>
    kProperties = {
        {"cx",
         [](CircleProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.cx);
         }},  //
        {"cy",
         [](CircleProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.cy);
         }},  //
        {"r",
         [](CircleProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.r);
         }}  //

};

}  // namespace

ComputedCircleComponent::ComputedCircleComponent(
    const CircleProperties& inputProperties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties,
    std::vector<ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError =
          it->second(properties, CreateParseFnParams(property.declaration, property.specificity));
      if (maybeError && outWarnings) {
        outWarnings->emplace_back(std::move(maybeError.value()));
      }
    }
  }
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Circle>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    CircleProperties& properties = handle.get_or_emplace<CircleComponent>().properties;
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

void InstantiateComputedCircleComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<CircleComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [circle, style] = view.get(entity);
    circle.computePathWithPrecomputedStyle(EntityHandle(registry, entity), style, FontMetrics(),
                                           outWarnings);
  }
}

}  // namespace donner::svg
