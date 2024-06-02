#include "src/svg/components/paint/stop_component.h"

#include "src/css/parser/color_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/properties/property_parsing.h"

namespace donner::svg::components {

namespace {

std::optional<ParseError> ParseStopColor(StopProperties& properties,
                                         const PropertyParseFnParams& params) {
  return Parse(
      params,
      [](const PropertyParseFnParams& params) {
        return css::ColorParser::Parse(params.components());
      },
      &properties.stopColor);
}

std::optional<ParseError> ParseStopOpacity(StopProperties& properties,
                                           const PropertyParseFnParams& params) {
  return Parse(
      params,
      [](const PropertyParseFnParams& params) { return ParseAlphaValue(params.components()); },
      &properties.stopOpacity);
}

// Returns true if the property was found and parsed successfully.
ParseResult<bool> ParseProperty(std::string_view name, const PropertyParseFnParams params,
                                StopProperties& properties) {
  // TODO: Case insensitive?
  if (name == "stop-color") {
    if (auto maybeError = ParseStopColor(properties, params)) {
      return std::move(maybeError.value());
    }
  } else if (name == "stop-opacity") {
    if (auto maybeError = ParseStopOpacity(properties, params)) {
      return std::move(maybeError.value());
    }
  } else {
    return false;
  }

  return true;
}

}  // namespace

ComputedStopComponent::ComputedStopComponent(
    const StopProperties& inputProperties, const ComputedStyleComponent& style,
    const std::map<RcString, UnparsedProperty>& unparsedProperties,
    std::vector<ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, unparsedProperty] : unparsedProperties) {
    const PropertyParseFnParams params =
        CreateParseFnParams(unparsedProperty.declaration, unparsedProperty.specificity,
                            PropertyParseBehavior::AllowUserUnits);

    auto result = ParseProperty(name, params, properties);
    if (result.hasError() && outWarnings) {
      outWarnings->emplace_back(std::move(result.error()));
    }
  }

  // Evaluate stopColor if it is currentColor.
  if (properties.stopColor.hasValue() && properties.stopColor.getRequired().isCurrentColor()) {
    const auto& currentColor = style.properties->color;
    properties.stopColor.set(currentColor.getRequired(), currentColor.specificity);
  }
}

}  // namespace donner::svg::components

namespace donner::svg {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Stop>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  components::StopProperties& properties =
      handle.get_or_emplace<components::StopComponent>().properties;
  return components::ParseProperty(name, params, properties);
}

}  // namespace donner::svg
