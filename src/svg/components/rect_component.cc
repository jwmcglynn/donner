#include "src/svg/components/rect_component.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

namespace {

using RectPresentationAttributeParseFn =
    std::optional<ParseError> (*)(RectProperties& properties, const PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, RectPresentationAttributeParseFn, 6>
    kProperties = {
        {"x",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components, params.allowUserUnits);
               },
               &properties.x);
         }},  //
        {"y",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components, params.allowUserUnits);
               },
               &properties.y);
         }},  //
        {"width",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components, params.allowUserUnits);
               },
               &properties.width);
         }},  //
        {"height",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components, params.allowUserUnits);
               },
               &properties.height);
         }},  //
        {"rx",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentageOrAuto(params.components, params.allowUserUnits);
               },
               &properties.rx);
         }},  //
        {"ry",
         [](RectProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentageOrAuto(params.components, params.allowUserUnits);
               },
               &properties.ry);
         }},  //
};

}  // namespace

ComputedRectComponent::ComputedRectComponent(
    const RectProperties& inputProperties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties)
    : properties(inputProperties) {
  for (const auto& [name, property] : unparsedProperties) {
    const auto it = kProperties.find(frozen::string(name));
    if (it != kProperties.end()) {
      auto maybeError =
          it->second(properties, CreateParseFnParams(property.declaration, property.specificity));
      if (maybeError) {
        std::cerr << "Error parsing property " << name << ": " << *maybeError << std::endl;
      }
    }
  }
}

void RectComponent::computePathWithPrecomputedStyle(EntityHandle handle,
                                                    const ComputedStyleComponent& style,
                                                    const FontMetrics& fontMetrics) {
  const ComputedRectComponent& computedRect = handle.get_or_emplace<ComputedRectComponent>(
      properties, style.properties().unparsedProperties);

  const Vector2d pos(
      computedRect.properties.x.getRequired().toPixels(style.viewbox(), fontMetrics),
      computedRect.properties.y.getRequired().toPixels(style.viewbox(), fontMetrics));
  const Vector2d size(
      computedRect.properties.width.getRequired().toPixels(style.viewbox(), fontMetrics),
      computedRect.properties.height.getRequired().toPixels(style.viewbox(), fontMetrics));

  if (computedRect.properties.rx.hasValue() || computedRect.properties.ry.hasValue()) {
    // 4/3 * (1 - cos(45 deg) / sin(45 deg) = 4/3 * (sqrt 2) - 1
    const double arcMagic = 0.5522847498;
    const Vector2d radius(
        computedRect.properties.calculateRx().toPixels(style.viewbox(), fontMetrics),
        computedRect.properties.calculateRy().toPixels(style.viewbox(), fontMetrics));

    // Draw with rounded corners.
    handle.get_or_emplace<ComputedPathComponent>().setSpline(
        PathSpline::Builder()
            // Draw top line.
            .moveTo(pos + Vector2d(radius.x, 0))
            .lineTo(pos + Vector2d(size.x - radius.x, 0.0))
            // Curve to the right line.
            .curveTo(pos + Vector2d(size.x - radius.x + radius.x * arcMagic, 0.0),
                     pos + Vector2d(size.x, radius.y - radius.y * arcMagic),
                     pos + Vector2d(size.x, radius.y))
            // Draw right line.
            .lineTo(pos + Vector2d(size.x, size.y - radius.y))
            // Curve to the bottom line.
            .curveTo(pos + Vector2d(size.x, size.y - radius.y + radius.y * arcMagic),
                     pos + Vector2d(size.x - radius.x + radius.x * arcMagic, size.y),
                     pos + Vector2d(size.x - radius.x, size.y))
            // Draw bottom line.
            .lineTo(pos + Vector2d(radius.x, size.y))
            // Curve to the left line.
            .curveTo(pos + Vector2d(radius.x - radius.x * arcMagic, size.y),
                     pos + Vector2d(0.0, size.y - radius.y + radius.y * arcMagic),
                     pos + Vector2d(0.0, size.y - radius.y))
            // Draw right line.
            .lineTo(pos + Vector2d(0.0, radius.y))
            // Curve to the top line.
            .curveTo(pos + Vector2d(0.0, radius.y - radius.y * arcMagic),
                     pos + Vector2d(radius.x - radius.x * arcMagic, 0.0),
                     pos + Vector2d(radius.x, 0.0))
            .closePath()
            .build());

  } else {
    handle.get_or_emplace<ComputedPathComponent>().setSpline(PathSpline::Builder()
                                                                 .moveTo(pos)
                                                                 .lineTo(pos + Vector2d(size.x, 0))
                                                                 .lineTo(pos + size)
                                                                 .lineTo(pos + Vector2d(0, size.y))
                                                                 .closePath()
                                                                 .build());
  }
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Rect>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    RectProperties& properties = handle.get_or_emplace<RectComponent>().properties;
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

}  // namespace donner::svg
