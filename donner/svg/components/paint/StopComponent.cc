#include "donner/svg/components/paint/StopComponent.h"

#include "donner/base/EcsRegistry.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

namespace {

std::optional<ParseDiagnostic> ParseStopColor(StopProperties& properties,
                                         const parser::PropertyParseFnParams& params) {
  return Parse(
      params,
      [](const parser::PropertyParseFnParams& params) {
        return css::parser::ColorParser::Parse(params.components());
      },
      &properties.stopColor);
}

std::optional<ParseDiagnostic> ParseStopOpacity(StopProperties& properties,
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
ParseResult<bool> ParseStopPresentationAttributeImpl(std::string_view name,
                                                     const parser::PropertyParseFnParams& params,
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
    ParseWarningSink& warningSink)
    : properties(inputProperties) {
  for (const auto& [name, unparsedProperty] : unparsedProperties) {
    const parser::PropertyParseFnParams params = parser::PropertyParseFnParams::Create(
        unparsedProperty.declaration, unparsedProperty.specificity,
        parser::PropertyParseBehavior::AllowUserUnits);

    auto result = ParseStopPresentationAttributeImpl(name, params, properties);
    if (result.hasError()) {
      warningSink.add(std::move(result.error()));
    }
  }

  // Evaluate stopColor if it is currentColor.
  if (properties.stopColor.hasValue() && properties.stopColor.getRequired().isCurrentColor()) {
    const auto& currentColor = style.properties->color;
    properties.stopColor.set(currentColor.getRequired(), currentColor.specificity);
  }
}

ParseResult<bool> ParseStopPresentationAttribute(EntityHandle handle, std::string_view name,
                                                  const parser::PropertyParseFnParams& params) {
  StopProperties& properties = handle.get_or_emplace<StopComponent>().properties;
  return ParseStopPresentationAttributeImpl(name, params, properties);
}

}  // namespace donner::svg::components
