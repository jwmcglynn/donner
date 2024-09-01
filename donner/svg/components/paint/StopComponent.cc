#include "donner/svg/components/paint/StopComponent.h"

#include "donner/css/parser/ColorParser.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, For ParsePresentationAttribute
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

std::optional<parser::ParseError> ParseStopColor(StopProperties& properties,
                                                 const parser::PropertyParseFnParams& params) {
  return Parse(
      params,
      [](const parser::PropertyParseFnParams& params) {
        return css::parser::ColorParser::Parse(params.components());
      },
      &properties.stopColor);
}

std::optional<parser::ParseError> ParseStopOpacity(StopProperties& properties,
                                                   const parser::PropertyParseFnParams& params) {
  return Parse(
      params,
      [](const parser::PropertyParseFnParams& params) {
        return parser::ParseAlphaValue(params.components());
      },
      &properties.stopOpacity);
}

/**
 * Parse presentation attributes for a \ref xml_stop element, such as `stop-color` and
 * `stop-opacity`.
 *
 * @param name Presentation attribute name (e.g. `stop-color`)
 * @param params Parameters for the property parsing function.
 * @param properties Properties storage for the parsed result.
 * @return True if the property was found and parsed successfully.
 */
parser::ParseResult<bool> ParseStopPresentationAttribute(
    std::string_view name, const parser::PropertyParseFnParams& params,
    StopProperties& properties) {
  // TODO(jwmcglynn): Case insensitive?
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
    const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
    std::vector<parser::ParseError>* outWarnings)
    : properties(inputProperties) {
  for (const auto& [name, unparsedProperty] : unparsedProperties) {
    const parser::PropertyParseFnParams params = parser::PropertyParseFnParams::Create(
        unparsedProperty.declaration, unparsedProperty.specificity,
        parser::PropertyParseBehavior::AllowUserUnits);

    auto result = ParseStopPresentationAttribute(name, params, properties);
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

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Stop>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  components::StopProperties& properties =
      handle.get_or_emplace<components::StopComponent>().properties;
  return components::ParseStopPresentationAttribute(name, params, properties);
}

}  // namespace donner::svg::parser
