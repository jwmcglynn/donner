#include "donner/svg/properties/PresentationAttributeParsing.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/parser/NumberParser.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg::parser {

namespace {

std::string_view ValueString(const PropertyParseFnParams& params) {
  if (const std::string_view* value = std::get_if<std::string_view>(&params.valueOrComponents)) {
    return *value;
  }

  return std::string_view();
}

ParseDiagnostic InvalidAttributeValue(std::string_view name, std::string_view value) {
  std::string reason = "Invalid " + std::string(name) + " value '" + std::string(value) + "'";
  return ParseDiagnostic::Error(RcString(reason), FileOffset::Offset(0));
}

ParseResult<bool> ParseFeColorMatrixPresentationAttribute(EntityHandle handle,
                                                          std::string_view name,
                                                          const PropertyParseFnParams& params) {
  auto& component = handle.get<components::FEColorMatrixComponent>();

  if (name == "type") {
    if (params.explicitState != PropertyState::NotSet) {
      component.type = components::FEColorMatrixComponent::Type::Matrix;
      return true;
    }

    const std::string_view value = ValueString(params);
    if (value == "matrix") {
      component.type = components::FEColorMatrixComponent::Type::Matrix;
    } else if (value == "saturate") {
      component.type = components::FEColorMatrixComponent::Type::Saturate;
    } else if (value == "hueRotate") {
      component.type = components::FEColorMatrixComponent::Type::HueRotate;
    } else if (value == "luminanceToAlpha") {
      component.type = components::FEColorMatrixComponent::Type::LuminanceToAlpha;
    } else {
      return InvalidAttributeValue(name, value);
    }

    return true;
  }

  if (name == "values") {
    if (params.explicitState != PropertyState::NotSet) {
      component.values.clear();
      return true;
    }

    std::vector<double> values;
    std::string_view remaining = ValueString(params);
    while (!remaining.empty()) {
      while (!remaining.empty() &&
             (remaining.front() == ' ' || remaining.front() == ',' || remaining.front() == '\t' ||
              remaining.front() == '\n' || remaining.front() == '\r')) {
        remaining.remove_prefix(1);
      }

      if (remaining.empty()) {
        break;
      }

      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (!maybeNumber.hasResult()) {
        return InvalidAttributeValue(name, ValueString(params));
      }

      values.push_back(maybeNumber.result().number);
      remaining.remove_prefix(maybeNumber.result().consumedChars);
    }

    component.values = std::move(values);
    return true;
  }

  return false;
}

}  // namespace

ParseResult<bool> ParsePresentationAttribute(ElementType type, EntityHandle handle,
                                             std::string_view name,
                                             const PropertyParseFnParams& params) {
  switch (type) {
    case ElementType::SVG:
    case ElementType::Use:
    case ElementType::Image:
      return components::ParseSizedElementPresentationAttribute(handle, name, params);

    case ElementType::Rect: return components::ParseRectPresentationAttribute(handle, name, params);
    case ElementType::Circle:
      return components::ParseCirclePresentationAttribute(handle, name, params);
    case ElementType::Ellipse:
      return components::ParseEllipsePresentationAttribute(handle, name, params);
    case ElementType::Path: return components::ParsePathPresentationAttribute(handle, name, params);

    case ElementType::Stop: return components::ParseStopPresentationAttribute(handle, name, params);

    case ElementType::FeFlood:
      return components::ParseFeFloodPresentationAttribute(handle, name, params);
    case ElementType::FeColorMatrix:
      return ParseFeColorMatrixPresentationAttribute(handle, name, params);
    case ElementType::FeDropShadow:
      return components::ParseFeDropShadowPresentationAttribute(handle, name, params);
    case ElementType::FeDiffuseLighting:
      return components::ParseFeDiffuseLightingPresentationAttribute(handle, name, params);
    case ElementType::FeSpecularLighting:
      return components::ParseFeSpecularLightingPresentationAttribute(handle, name, params);

    // All other elements have no element-specific presentation attributes.
    default: return false;
  }
}

}  // namespace donner::svg::parser
