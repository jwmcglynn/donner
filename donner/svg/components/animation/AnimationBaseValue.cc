#include "donner/svg/components/animation/AnimationBaseValue.h"

#include <sstream>
#include <type_traits>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
namespace {

template <typename T>
std::optional<std::string> SerializeValue(const T& value, const AnimationValueContext& context) {
  if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, Lengthd>) {
    std::ostringstream out;
    out.precision(12);
    out << value;
    return out.str();
  } else if constexpr (std::is_same_v<T, css::Color>) {
    return value.resolve(context.currentColor, 1).toHexString();
  } else if constexpr (std::is_same_v<T, PaintServer>) {
    if (value.template is<PaintServer::Solid>()) {
      return value.template get<PaintServer::Solid>()
          .color.resolve(context.currentColor, 1)
          .toHexString();
    }
    if (value.template is<PaintServer::None>()) {
      return "none";
    }
  }
  return std::nullopt;
}

std::optional<std::string> StyleValue(const PropertyRegistry& properties,
                                      std::string_view attribute,
                                      const AnimationValueContext& context) {
  std::optional<std::string> result;
  const auto all = properties.allProperties();
  PropertyRegistry::forEachProperty<0, PropertyRegistry::numProperties()>([&](auto index) {
    const auto& property = std::get<index>(all);
    if (property.name == attribute) {
      if (const auto value = property.get()) {
        result = SerializeValue(*value, context);
      }
    }
  });
  return result;
}

}  // namespace

AnimationValueContext AnimationContextFor(Registry& registry, Entity target,
                                          std::string_view attribute) {
  AnimationValueContext result;
  result.viewBox = LayoutSystem().getViewBox(EntityHandle(registry, target));
  if (attribute == "x" || attribute == "dx" || attribute == "x1" || attribute == "x2" ||
      attribute == "cx" || attribute == "rx" || attribute == "width") {
    result.extent = Lengthd::Extent::X;
  }
  if (attribute == "y" || attribute == "dy" || attribute == "y1" || attribute == "y2" ||
      attribute == "cy" || attribute == "ry" || attribute == "height") {
    result.extent = Lengthd::Extent::Y;
  }
  const auto& document = registry.ctx().get<SVGDocumentContext>();
  if (document.canvasSize) {
    result.fontMetrics.viewportSize = Vector2d(*document.canvasSize);
  }
  if (const auto* style = registry.try_get<ComputedStyleComponent>(target);
      style && style->properties) {
    result.linearRgb = style->properties->colorInterpolation.getOr(ColorInterpolation::SRGB) ==
                       ColorInterpolation::LinearRGB;
    if (const auto color = style->properties->color.get()) {
      result.currentColor = color->resolve(result.currentColor, 1);
    }
    if (const auto font = style->properties->fontSize.get()) {
      result.fontMetrics.fontSize = font->toPixels(result.viewBox, result.fontMetrics);
    }
  }
  if (const auto* rootStyle = registry.try_get<ComputedStyleComponent>(document.rootEntity);
      rootStyle && rootStyle->properties) {
    if (const auto font = rootStyle->properties->fontSize.get()) {
      result.fontMetrics.rootFontSize = font->toPixels(result.viewBox, result.fontMetrics);
    }
  }
  return result;
}

std::string UnderlyingAnimationValue(Registry& registry, Entity target, std::string_view attribute,
                                     const AnimationValueContext& context,
                                     AnimationResourceBudget& budget) {
  if (const auto* animated = registry.try_get<AnimatedValuesComponent>(target)) {
    if (const auto it = animated->overrides.find(std::string(attribute));
        it != animated->overrides.end()) {
      return budget.reserveSourceBytes(it->second.size()) ? it->second : std::string{};
    }
  }

  if (attribute == "transform" || attribute == "gradientTransform" ||
      attribute == "patternTransform") {
    if (const auto* transform = registry.try_get<TransformComponent>(target);
        transform && transform->transform.specificity == css::Specificity::Override()) {
      if (const auto* value = transform->transform.getStoredValue()) {
        return toSVGTransformString(value->compute(context.viewBox, context.fontMetrics)).str();
      }
    }
  }
  if (const auto* style = registry.try_get<ComputedStyleComponent>(target);
      style && style->properties) {
    if (const auto value = StyleValue(*style->properties, attribute, context)) {
      return *value;
    }
    const auto it = style->properties->unparsedProperties.find(RcString(attribute));
    if (it != style->properties->unparsedProperties.end()) {
      const std::size_t bytes = style->properties->complexPropertyBytes();
      if (bytes > AnimationResourceBudget::kMaximumSourceBytes / 6 ||
          !budget.reserveSourceBytes(bytes * 6)) {
        return {};
      }
      std::string value;
      for (const auto& component : it->second.declaration.values) {
        value += component.toCssText();
      }
      return value;
    }
  }
  if (const auto* attributes = registry.try_get<donner::components::AttributesComponent>(target)) {
    if (const auto value = attributes->getAttribute(xml::XMLQualifiedNameRef(attribute))) {
      return budget.reserveSourceBytes(value->size()) ? value->str() : std::string{};
    }
  }
  if (attribute == "transform" || attribute == "gradientTransform" ||
      attribute == "patternTransform") {
    return {};
  }
  if (attribute == "fill" || attribute == "color") {
    return "black";
  }
  if (attribute == "stroke") {
    return "none";
  }
  if (attribute == "opacity" || attribute == "fill-opacity" || attribute == "stroke-opacity") {
    return "1";
  }
  return "0";
}

}  // namespace donner::svg::components
