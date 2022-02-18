#include "src/svg/components/sized_element_component.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/svg/components/viewbox_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

namespace {

using SizedElementPresentationAttributeParseFn = std::optional<ParseError> (*)(
    SizedElementProperties& properties, const PropertyParseFnParams& params);

static constexpr frozen::unordered_map<frozen::string, SizedElementPresentationAttributeParseFn, 4>
    kProperties = {
        {"x",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.x);
         }},  //
        {"y",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.y);
         }},  //
        {"width",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.width);
         }},  //
        {"height",
         [](SizedElementProperties& properties, const PropertyParseFnParams& params) {
           return Parse(
               params,
               [](const PropertyParseFnParams& params) {
                 return ParseLengthPercentage(params.components(), params.allowUserUnits);
               },
               &properties.height);
         }},
};

ParseResult<bool> ParseSizedElementPresentationAttribute(EntityHandle handle, std::string_view name,
                                                         const PropertyParseFnParams& params) {
  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    SizedElementProperties& properties = handle.get_or_emplace<SizedElementComponent>().properties;
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

Boxd calculateBounds(EntityHandle handle, const SizedElementProperties& properties,
                     const Boxd& inheritedViewbox, FontMetrics fontMetrics) {
  // TODO: Confirm if this is the correct behavior if <svg> has a viewbox specifying a size, but
  // no width/height. For Ghostscript_Tiger to detect the size, we need to do this.
  Vector2d size;
  if (const auto* viewbox = handle.try_get<ViewboxComponent>();
      viewbox && !properties.width.hasValue() && !properties.height.hasValue() &&
      viewbox->viewbox) {
    size = viewbox->viewbox->size();
  } else {
    size = inheritedViewbox.size();
  }

  if (properties.width.hasValue()) {
    size.x =
        properties.width.getRequired().toPixels(inheritedViewbox, fontMetrics, Lengthd::Extent::X);
  }

  if (properties.height.hasValue()) {
    size.y =
        properties.height.getRequired().toPixels(inheritedViewbox, fontMetrics, Lengthd::Extent::Y);
  }

  const Vector2d origin(
      properties.x.getRequired().toPixels(Boxd(Vector2d(), size), fontMetrics, Lengthd::Extent::X),
      properties.y.getRequired().toPixels(Boxd(Vector2d(), size), fontMetrics, Lengthd::Extent::Y));
  return Boxd(origin, origin + size);
}

void applyUnparsedProperties(SizedElementProperties& properties,
                             const std::map<RcString, UnparsedProperty>& unparsedProperties,
                             std::vector<ParseError>* outWarnings) {
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

Boxd applyUnparsedPropertiesAndGetBounds(
    EntityHandle handle, SizedElementProperties& properties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties, Boxd inheritedViewbox,
    FontMetrics fontMetrics, std::vector<ParseError>* outWarnings) {
  applyUnparsedProperties(properties, unparsedProperties, outWarnings);
  return calculateBounds(handle, properties, inheritedViewbox, fontMetrics);
}

}  // namespace

ComputedSizedElementComponent::ComputedSizedElementComponent(
    EntityHandle handle, SizedElementProperties properties,
    const std::map<RcString, UnparsedProperty>& unparsedProperties, Boxd inheritedViewbox,
    FontMetrics fontMetrics, std::vector<ParseError>* outWarnings)
    : bounds(applyUnparsedPropertiesAndGetBounds(handle, properties, unparsedProperties,
                                                 inheritedViewbox, fontMetrics, outWarnings)),
      inheritedViewbox(inheritedViewbox) {}

std::optional<Boxd> ComputedSizedElementComponent::clipRect(EntityHandle handle) const {
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    return bounds;
  }

  return std::nullopt;
}

Transformd ComputedSizedElementComponent::computeTransform(EntityHandle handle) const {
  // If this entity also has a viewbox, this SizedElementComponent is used to define a viewport.
  if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
    return viewbox->computeTransform(
        bounds, handle.get<PreserveAspectRatioComponent>().preserveAspectRatio);
  } else {
    PreserveAspectRatio preserveAspectRatio;
    if (const auto* preserveAspectRatioComponent = handle.try_get<PreserveAspectRatioComponent>()) {
      preserveAspectRatio = preserveAspectRatioComponent->preserveAspectRatio;
    }

    Vector2d scale = bounds.size() / inheritedViewbox.size();

    if (preserveAspectRatio.align != PreserveAspectRatio::Align::None) {
      if (preserveAspectRatio.meetOrSlice == PreserveAspectRatio::MeetOrSlice::Meet) {
        scale.x = scale.y = std::min(scale.x, scale.y);
      } else {
        scale.x = scale.y = std::max(scale.x, scale.y);
      }
    }

    Vector2d translation = bounds.top_left - (inheritedViewbox.top_left * scale);
    const Vector2d alignMaxOffset = bounds.size() - inheritedViewbox.size() * scale;

    const Vector2d alignMultiplier(preserveAspectRatio.alignMultiplierX(),
                                   preserveAspectRatio.alignMultiplierY());
    return Transformd::Scale(scale) *
           Transformd::Translate(translation + alignMaxOffset * alignMultiplier);
  }
}

void SizedElementComponent::computeWithPrecomputedStyle(EntityHandle handle,
                                                        const ComputedStyleComponent& style,
                                                        FontMetrics fontMetrics,
                                                        std::vector<ParseError>* outWarnings) {
  handle.emplace_or_replace<ComputedSizedElementComponent>(
      handle, properties, style.properties().unparsedProperties, style.viewbox(), fontMetrics,
      outWarnings);
}

// SVGSVGElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::SVG>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return ParseSizedElementPresentationAttribute(handle, name, params);
}

// SVGUseElement shares this component.
template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Use>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  return ParseSizedElementPresentationAttribute(handle, name, params);
}

}  // namespace donner::svg
