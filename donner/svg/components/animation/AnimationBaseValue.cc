#include "donner/svg/components/animation/AnimationBaseValue.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"

namespace donner::svg::components {
namespace {

template <typename T>
std::optional<std::string> SerializeValue(const T& value, const AnimationValueContext& context) {
  if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, Lengthd> || std::is_enum_v<T>) {
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

template <typename Properties>
std::optional<std::string> PropertyValue(Properties& properties, std::string_view attribute,
                                         const AnimationValueContext& context,
                                         bool overridesOnly = false) {
  std::optional<std::string> result;
  auto inspect = [&](const auto& property) {
    if (property.name == attribute &&
        (!overridesOnly || property.specificity == css::Specificity::Override())) {
      if (const auto value = property.get()) {
        result = SerializeValue(*value, context);
      }
    }
  };
  std::apply([&](const auto&... property) { (inspect(property), ...); },
             properties.allProperties());
  if constexpr (requires { properties.calculateRx(context.viewBox, context.fontMetrics); }) {
    if (!result && !overridesOnly && attribute == "rx") {
      result = SerializeValue(
          std::get<0>(properties.calculateRx(context.viewBox, context.fontMetrics)), context);
    }
    if (!result && !overridesOnly && attribute == "ry") {
      result = SerializeValue(
          std::get<0>(properties.calculateRy(context.viewBox, context.fontMetrics)), context);
    }
  }
  return result;
}

template <typename Component, typename Computed>
std::optional<std::string> GeometryPropertyValue(Registry& registry, Entity target,
                                                 std::string_view attribute,
                                                 const AnimationValueContext& context,
                                                 AnimationResourceBudget& budget) {
  auto* component = registry.try_get<Component>(target);
  if (!component) {
    return std::nullopt;
  }
  const auto* style = registry.try_get<ComputedStyleComponent>(target);
  if (!style || !style->properties) {
    return PropertyValue(component->properties, attribute, context);
  }
  const auto bytes = style->properties->complexPropertyBytes();
  if (bytes > AnimationResourceBudget::kMaximumSourceBytes / 6 ||
      !budget.reserveSourceBytes(bytes * 6)) {
    return std::string{};
  }
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  Computed computed(component->properties, style->properties->unparsedProperties, warnings);
  return PropertyValue(computed.properties, attribute, context);
}

std::optional<std::string> GeometryBaseValue(Registry& registry, Entity target,
                                             std::string_view attribute,
                                             const AnimationValueContext& context,
                                             AnimationResourceBudget& budget) {
  static constexpr std::array<std::string_view, 9> names = {"x",  "y", "width", "height", "cx",
                                                            "cy", "r", "rx",    "ry"};
  if (std::find(names.begin(), names.end(), attribute) == names.end()) {
    return std::nullopt;
  }
  if (auto result = GeometryPropertyValue<RectComponent, ComputedRectComponent>(
          registry, target, attribute, context, budget)) {
    return result;
  }
  if (auto result = GeometryPropertyValue<CircleComponent, ComputedCircleComponent>(
          registry, target, attribute, context, budget)) {
    return result;
  }
  return GeometryPropertyValue<EllipseComponent, ComputedEllipseComponent>(
      registry, target, attribute, context, budget);
}

template <typename Component, typename Value>
std::optional<Value> InheritedGradientField(Registry& registry, Entity target,
                                            std::optional<Value> Component::*field,
                                            AnimationResourceBudget& budget) {
  std::unordered_set<Entity> visited;
  while (budget.reserveGeometryWork(1) && visited.insert(target).second) {
    const auto* component = registry.try_get<Component>(target);
    if (!component || !IsAnimationInSvgNamespace(registry, target)) {
      return std::nullopt;
    }
    if (component->*field) {
      return component->*field;
    }
    const auto href = AnimationReferenceValue(registry, target);
    if (!href || !budget.reserveSourceBytes(href->size())) {
      return std::nullopt;
    }
    const auto reference = Reference(*href).resolve(registry);
    if (!reference) {
      return std::nullopt;
    }
    target = reference->handle.entity();
  }
  return std::nullopt;
}

std::optional<Lengthd> LinearGradientBaseLength(Registry& registry, Entity target,
                                                std::string_view name,
                                                AnimationResourceBudget& budget) {
  if (!registry.all_of<LinearGradientComponent>(target)) {
    return std::nullopt;
  }
  using Field = std::optional<Lengthd> LinearGradientComponent::*;
  struct Attribute {
    std::string_view name;
    Field field;
    Lengthd fallback;
  };
  const ComputedLinearGradientComponent defaults;
  const Attribute attributes[] = {{"x1", &LinearGradientComponent::x1, defaults.x1},
                                  {"y1", &LinearGradientComponent::y1, defaults.y1},
                                  {"x2", &LinearGradientComponent::x2, defaults.x2},
                                  {"y2", &LinearGradientComponent::y2, defaults.y2}};
  for (const auto& item : attributes) {
    if (item.name == name) {
      return InheritedGradientField(registry, target, item.field, budget).value_or(item.fallback);
    }
  }
  return std::nullopt;
}

std::optional<Lengthd> RadialGradientBaseLength(Registry& registry, Entity target,
                                                std::string_view name,
                                                AnimationResourceBudget& budget) {
  if (!registry.all_of<RadialGradientComponent>(target)) {
    return std::nullopt;
  }
  const ComputedRadialGradientComponent defaults;
  using Field = std::optional<Lengthd> RadialGradientComponent::*;
  struct Attribute {
    std::string_view name;
    Field field;
    Lengthd fallback;
  };
  const Attribute attributes[] = {{"cx", &RadialGradientComponent::cx, defaults.cx},
                                  {"cy", &RadialGradientComponent::cy, defaults.cy},
                                  {"r", &RadialGradientComponent::r, defaults.r},
                                  {"fr", &RadialGradientComponent::fr, defaults.fr}};
  for (const auto& item : attributes) {
    if (item.name == name) {
      return InheritedGradientField(registry, target, item.field, budget).value_or(item.fallback);
    }
  }
  if (name == "fx" || name == "fy") {
    const auto field = name == "fx" ? &RadialGradientComponent::fx : &RadialGradientComponent::fy;
    if (auto value = InheritedGradientField(registry, target, field, budget)) {
      return value;
    }
    return RadialGradientBaseLength(registry, target, name == "fx" ? "cx" : "cy", budget);
  }
  return std::nullopt;
}

std::optional<std::string> GradientBaseValue(Registry& registry, Entity target,
                                             std::string_view attribute,
                                             const AnimationValueContext& context,
                                             AnimationResourceBudget& budget) {
  auto length = LinearGradientBaseLength(registry, target, attribute, budget);
  if (!length) {
    length = RadialGradientBaseLength(registry, target, attribute, budget);
  }
  return length ? SerializeValue(*length, context) : std::nullopt;
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

template <typename Component>
std::optional<RcString> ReadReference(Registry& registry, Entity entity) {
  const auto* component = registry.try_get<Component>(entity);
  if (!component) {
    return std::nullopt;
  }
  if constexpr (std::is_same_v<decltype(component->href), RcString>) {
    return component->href;
  } else {
    return component->href ? std::optional(component->href->href) : std::nullopt;
  }
}

}  // namespace

std::optional<RcString> AnimationReferenceValue(Registry& registry, Entity entity) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  switch (type ? type->type() : ElementType::Unknown) {
    case ElementType::Use: {
      const auto* shadow = registry.try_get<ShadowTreeComponent>(entity);
      return shadow ? shadow->mainHref() : std::nullopt;
    }
    case ElementType::LinearGradient:
    case ElementType::RadialGradient: return ReadReference<GradientComponent>(registry, entity);
    case ElementType::Pattern: return ReadReference<PatternComponent>(registry, entity);
    case ElementType::Filter: return ReadReference<FilterComponent>(registry, entity);
    case ElementType::Image: return ReadReference<ImageComponent>(registry, entity);
    case ElementType::FeImage: return ReadReference<FEImageComponent>(registry, entity);
    case ElementType::TextPath: return ReadReference<TextPathComponent>(registry, entity);
    default: {
      const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
      if (!attributes) {
        return std::nullopt;
      }
      if (auto href = attributes->getAttribute("href")) {
        return href;
      }
      return attributes->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
    }
  }
}

AnimationValueContext AnimationContextFor(Registry& registry, Entity target,
                                          std::string_view attribute,
                                          AnimationResourceBudget& budget) {
  AnimationValueContext result;
  result.referenceValue = attribute == "href" || attribute == "xlink:href";
  result.viewBox = LayoutSystem().getViewBox(EntityHandle(registry, target));
  if (registry.all_of<GradientComponent>(target) &&
      InheritedGradientField(registry, target, &GradientComponent::gradientUnits, budget)
              .value_or(GradientUnits::Default) == GradientUnits::ObjectBoundingBox) {
    result.viewBox = Box2d{{0, 0}, {1, 1}};
  }
  if (attribute == "x" || attribute == "dx" || attribute == "x1" || attribute == "x2" ||
      attribute == "cx" || attribute == "rx" || attribute == "width" || attribute == "fx" ||
      attribute == "refX" || attribute == "markerWidth") {
    result.extent = Lengthd::Extent::X;
  }
  if (attribute == "y" || attribute == "dy" || attribute == "y1" || attribute == "y2" ||
      attribute == "cy" || attribute == "ry" || attribute == "height" || attribute == "fy" ||
      attribute == "refY" || attribute == "markerHeight") {
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

  if (auto value = GeometryBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto value = GradientBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto* sized = registry.try_get<SizedElementComponent>(target)) {
    if (auto value = PropertyValue(sized->properties, attribute, context, true)) {
      return *value;
    }
  }
  if (context.referenceValue) {
    const auto value = AnimationReferenceValue(registry, target);
    return value && budget.reserveSourceBytes(value->size()) ? value->str() : std::string{};
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
  if (auto* sized = registry.try_get<SizedElementComponent>(target)) {
    if (auto value = PropertyValue(sized->properties, attribute, context)) {
      return *value;
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
