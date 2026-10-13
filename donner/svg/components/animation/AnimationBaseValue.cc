#include "donner/svg/components/animation/AnimationBaseValue.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationNumericAttributes.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
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

std::optional<std::string> SerializeReference(const Reference& reference,
                                              AnimationResourceBudget* budget) {
  const std::string_view href(reference.href);
  if (!budget || !reference.hasSafeRepresentation()) {
    return std::nullopt;
  }
  if (!budget->reserveSourceBytes(href.size() * 4 + 32)) {
    return std::string{};
  }
  std::string result = "url(\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char character : href) {
    if (character < 32 || character == 127) {
      result += '\\';
      result += hex[character >> 4];
      result += hex[character & 15];
      result += ' ';
    } else {
      if (character == '\\' || character == '"') {
        result += '\\';
      }
      result += static_cast<char>(character);
    }
  }
  result += "\")";
  return result;
}

std::optional<std::string> SerializeDasharray(const StrokeDasharray& value,
                                              AnimationResourceBudget* budget) {
  if (value.empty()) {
    return "none";
  }
  if (!budget || value.size() > AnimationResourceBudget::kMaximumNumbersPerValue ||
      !budget->reserveSourceBytes(value.size() * 64)) {
    return std::string{};
  }
  std::ostringstream out;
  out.precision(12);
  out << value;
  return out.str();
}

template <typename T>
std::optional<std::string> SerializeValue(const T& value, const AnimationValueContext& context,
                                          AnimationResourceBudget* budget = nullptr) {
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
    if (value.template is<PaintServer::ContextFill>()) {
      return "context-fill";
    }
    if (value.template is<PaintServer::ContextStroke>()) {
      return "context-stroke";
    }
    const auto& paint = value.template get<PaintServer::ElementReference>();
    auto result = SerializeReference(paint.reference, budget);
    if (result && paint.fallback) {
      *result += " " + paint.fallback->resolve(context.currentColor, 1).toHexString();
    }
    return result;
  } else if constexpr (std::is_same_v<T, Reference>) {
    return SerializeReference(value, budget);
  } else if constexpr (std::is_same_v<T, StrokeDasharray>) {
    return SerializeDasharray(value, budget);
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

std::optional<bool> SampledBoundingBoxUnits(Registry& registry, Entity target,
                                            std::string_view attribute) {
  const auto* values = registry.try_get<AnimatedValuesComponent>(target);
  if (!values) {
    return std::nullopt;
  }
  const auto it = values->overrides.find(std::string(attribute));
  if (it == values->overrides.end()) {
    return std::nullopt;
  }
  if (it->second == "userSpaceOnUse") {
    return false;
  }
  if (it->second == "objectBoundingBox") {
    return true;
  }
  return std::nullopt;
}

std::optional<RcString> InheritanceReference(Registry& registry, Entity target,
                                             AnimationResourceBudget& budget) {
  if (const auto* values = registry.try_get<AnimatedValuesComponent>(target)) {
    auto it = values->overrides.find("href");
    const auto* attributes = registry.try_get<donner::components::AttributesComponent>(target);
    if (it == values->overrides.end() && (!attributes || !attributes->getAttribute("href"))) {
      it = values->overrides.find("xlink:href");
    }
    if (it != values->overrides.end()) {
      if (!budget.reserveSourceBytes(it->second.size())) {
        return std::nullopt;
      }
      RcString href(it->second);
      if (Reference(href).hasSafeRepresentation()) {
        return href;
      }
    }
  }
  const auto href = AnimationReferenceValue(registry, target);
  return href && budget.reserveSourceBytes(href->size()) ? href : std::nullopt;
}

template <typename Component, typename Value, typename Reader>
std::optional<Value> InheritedResourceValue(Registry& registry, Entity target,
                                            AnimationResourceBudget& budget, Reader read) {
  std::unordered_set<Entity> visited;
  while (budget.reserveGeometryWork(1) && visited.insert(target).second) {
    const auto* component = registry.try_get<Component>(target);
    if (!component || !IsAnimationInSvgNamespace(registry, target)) {
      return std::nullopt;
    }
    if (auto value = read(*component, target)) {
      return value;
    }
    const auto href = InheritanceReference(registry, target, budget);
    if (!href) {
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

template <typename Component, typename Value>
std::optional<Value> InheritedResourceField(Registry& registry, Entity target,
                                            std::optional<Value> Component::*field,
                                            AnimationResourceBudget& budget,
                                            std::string_view unitAttribute = {}) {
  return InheritedResourceValue<Component, Value>(
      registry, target, budget,
      [&](const Component& component, Entity entity) -> std::optional<Value> {
        if constexpr (requires {
                        Value::ObjectBoundingBox;
                        Value::UserSpaceOnUse;
                      }) {
          if (const auto boxUnits = SampledBoundingBoxUnits(registry, entity, unitAttribute)) {
            return *boxUnits ? Value::ObjectBoundingBox : Value::UserSpaceOnUse;
          }
        }
        return component.*field;
      });
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
      return InheritedResourceField(registry, target, item.field, budget).value_or(item.fallback);
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
      return InheritedResourceField(registry, target, item.field, budget).value_or(item.fallback);
    }
  }
  if (name == "fx" || name == "fy") {
    const auto field = name == "fx" ? &RadialGradientComponent::fx : &RadialGradientComponent::fy;
    if (auto value = InheritedResourceField(registry, target, field, budget)) {
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

std::optional<std::string> PatternBaseValue(Registry& registry, Entity target,
                                            std::string_view name,
                                            const AnimationValueContext& context,
                                            AnimationResourceBudget& budget) {
  if (!registry.all_of<PatternComponent>(target)) {
    return std::nullopt;
  }
  using Field = Property<Lengthd> SizedElementProperties::*;
  const std::pair<std::string_view, Field> fields[] = {{"x", &SizedElementProperties::x},
                                                       {"y", &SizedElementProperties::y},
                                                       {"width", &SizedElementProperties::width},
                                                       {"height", &SizedElementProperties::height}};
  for (const auto& [attribute, field] : fields) {
    if (attribute != name) {
      continue;
    }
    SizedElementProperties properties;
    const auto value = InheritedResourceValue<PatternComponent, Property<Lengthd>>(
        registry, target, budget,
        [field, &budget](const PatternComponent& pattern,
                         Entity) -> std::optional<Property<Lengthd>> {
          const auto& property = pattern.sizeProperties.*field;
          if (!property.isSpecified() || !budget.reserveSourceBytes(sizeof(property))) {
            return std::nullopt;
          }
          return property;
        });
    if (value) {
      properties.*field = *value;
    }
    if (const auto* style = registry.try_get<ComputedStyleComponent>(target);
        style && style->properties) {
      const auto bytes = style->properties->complexPropertyBytes();
      if (bytes > AnimationResourceBudget::kMaximumSourceBytes / 6 ||
          !budget.reserveSourceBytes(bytes * 6)) {
        return std::string{};
      }
      ParseWarningSink warnings = ParseWarningSink::Disabled();
      properties = LayoutSystem().resolveSizeProperties(
          properties, style->properties->unparsedProperties, warnings);
    }
    return SerializeValue((properties.*field).get().value_or(Lengthd()), context);
  }
  return std::nullopt;
}

std::optional<std::string> FilterRegionBaseValue(Registry& registry, Entity target,
                                                 std::string_view name,
                                                 const AnimationValueContext& context,
                                                 AnimationResourceBudget& budget) {
  if (!registry.all_of<FilterComponent>(target)) {
    return std::nullopt;
  }
  using Field = std::optional<Lengthd> FilterComponent::*;
  const std::pair<std::string_view, Field> fields[] = {{"x", &FilterComponent::x},
                                                       {"y", &FilterComponent::y},
                                                       {"width", &FilterComponent::width},
                                                       {"height", &FilterComponent::height}};
  for (const auto& [attribute, field] : fields) {
    if (attribute == name) {
      const Lengthd fallback(name == "x" || name == "y" ? -10 : 120, Lengthd::Unit::Percent);
      return SerializeValue(
          InheritedResourceField(registry, target, field, budget).value_or(fallback), context);
    }
  }
  return std::nullopt;
}

bool ResourceUsesBoundingBoxUnits(Registry& registry, Entity target,
                                  AnimationResourceBudget& budget) {
  if (registry.all_of<GradientComponent>(target)) {
    return InheritedResourceField(registry, target, &GradientComponent::gradientUnits, budget,
                                  "gradientUnits")
               .value_or(GradientUnits::Default) == GradientUnits::ObjectBoundingBox;
  }
  if (registry.all_of<PatternComponent>(target)) {
    return InheritedResourceField(registry, target, &PatternComponent::patternUnits, budget,
                                  "patternUnits")
               .value_or(PatternUnits::Default) == PatternUnits::ObjectBoundingBox;
  }
  if (registry.all_of<FilterComponent>(target)) {
    return InheritedResourceField(registry, target, &FilterComponent::filterUnits, budget,
                                  "filterUnits")
               .value_or(FilterUnits::Default) == FilterUnits::ObjectBoundingBox;
  }
  if (registry.all_of<FilterPrimitiveComponent>(target)) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(target);
    if (tree && registry.all_of<FilterComponent>(tree->parent())) {
      return InheritedResourceField(registry, tree->parent(), &FilterComponent::primitiveUnits,
                                    budget, "primitiveUnits")
                 .value_or(PrimitiveUnits::Default) == PrimitiveUnits::ObjectBoundingBox;
    }
  }
  if (const auto* mask = registry.try_get<MaskComponent>(target)) {
    return SampledBoundingBoxUnits(registry, target, "maskUnits")
        .value_or(mask->maskUnits == MaskUnits::ObjectBoundingBox);
  }
  return false;
}

std::string NumberPair(double first, double second) {
  std::ostringstream output;
  output.precision(12);
  output << first << ' ' << second;
  return output.str();
}

template <typename Component>
std::optional<std::string> LayoutLengthBaseValue(
    Registry& registry, Entity target, std::string_view name,
    std::initializer_list<std::pair<std::string_view, Lengthd Component::*>> fields,
    const AnimationValueContext& context) {
  const auto* component = registry.try_get<Component>(target);
  if (!component) {
    return std::nullopt;
  }
  for (const auto& [attribute, field] : fields) {
    if (attribute == name) {
      return SerializeValue(component->*field, context);
    }
  }
  return std::nullopt;
}

std::optional<std::string> LayoutBaseValue(Registry& registry, Entity target, std::string_view name,
                                           const AnimationValueContext& context) {
  if (const auto* textPath = registry.try_get<TextPathComponent>(target);
      textPath && name == "startOffset") {
    return SerializeValue(textPath->startOffset.value_or(Lengthd()), context);
  }
  if (const auto* primitive = registry.try_get<FilterPrimitiveComponent>(target)) {
    using Field = std::optional<Lengthd> FilterPrimitiveComponent::*;
    const std::pair<std::string_view, Field> fields[] = {
        {"x", &FilterPrimitiveComponent::x},
        {"y", &FilterPrimitiveComponent::y},
        {"width", &FilterPrimitiveComponent::width},
        {"height", &FilterPrimitiveComponent::height}};
    for (const auto& [attribute, field] : fields) {
      if (attribute == name && (primitive->*field).has_value()) {
        return SerializeValue(*(primitive->*field), context);
      }
    }
  }
  if (auto value =
          LayoutLengthBaseValue<MarkerComponent>(registry, target, name,
                                                 {{"markerWidth", &MarkerComponent::markerWidth},
                                                  {"markerHeight", &MarkerComponent::markerHeight},
                                                  {"refX", &MarkerComponent::refX},
                                                  {"refY", &MarkerComponent::refY}},
                                                 context)) {
    return value;
  }
  if (auto value = LayoutLengthBaseValue<SymbolComponent>(
          registry, target, name,
          {{"refX", &SymbolComponent::refX}, {"refY", &SymbolComponent::refY}}, context)) {
    return value;
  }
  if (const auto* mask = registry.try_get<MaskComponent>(target)) {
    using Field = std::optional<Lengthd> MaskComponent::*;
    const std::pair<std::string_view, Field> fields[] = {{"x", &MaskComponent::x},
                                                         {"y", &MaskComponent::y},
                                                         {"width", &MaskComponent::width},
                                                         {"height", &MaskComponent::height}};
    for (const auto& [attribute, field] : fields) {
      if (attribute == name) {
        const Lengthd fallback(name == "x" || name == "y" ? -10 : 120, Lengthd::Unit::Percent);
        return SerializeValue((mask->*field).value_or(fallback), context);
      }
    }
  }
  return std::nullopt;
}

std::string FilterInputBaseValue(const std::optional<FilterInput>& input,
                                 AnimationResourceBudget& budget) {
  if (!input || std::holds_alternative<FilterInput::Previous>(input->value)) {
    // An empty underlying sample leaves the implicit previous-input binding untouched.
    return {};
  }
  if (const auto* named = std::get_if<FilterInput::Named>(&input->value)) {
    return budget.reserveSourceBytes(named->name.size()) ? named->name.str() : std::string{};
  }
  switch (std::get<FilterStandardInput>(input->value)) {
    case FilterStandardInput::SourceGraphic: return "SourceGraphic";
    case FilterStandardInput::SourceAlpha: return "SourceAlpha";
    case FilterStandardInput::FillPaint: return "FillPaint";
    case FilterStandardInput::StrokePaint: return "StrokePaint";
  }
  return {};
}

std::optional<std::string> FilterReferenceBaseValue(Registry& registry, Entity target,
                                                    std::string_view name,
                                                    AnimationResourceBudget& budget) {
  if (const auto* primitive = registry.try_get<FilterPrimitiveComponent>(target)) {
    if (name == "in" || name == "in2") {
      return FilterInputBaseValue(name == "in" ? primitive->in : primitive->in2, budget);
    }
    if (name == "result") {
      return primitive->result && budget.reserveSourceBytes(primitive->result->size())
                 ? primitive->result->str()
                 : std::string{};
    }
  }
  if (name == "in") {
    if (const auto* merge = registry.try_get<FEMergeNodeComponent>(target)) {
      return FilterInputBaseValue(merge->in, budget);
    }
  }
  return std::nullopt;
}

std::optional<std::string> FilterPairBaseValue(Registry& registry, Entity target,
                                               std::string_view name) {
  if (name == "stdDeviation") {
    if (const auto* blur = registry.try_get<FEGaussianBlurComponent>(target)) {
      return NumberPair(blur->stdDeviationX, blur->stdDeviationY);
    }
    if (const auto* shadow = registry.try_get<FEDropShadowComponent>(target)) {
      return NumberPair(shadow->stdDeviationX, shadow->stdDeviationY);
    }
  }
  if (name == "radius") {
    if (const auto* morph = registry.try_get<FEMorphologyComponent>(target)) {
      return NumberPair(morph->radiusX, morph->radiusY);
    }
  }
  if (name == "baseFrequency") {
    if (const auto* noise = registry.try_get<FETurbulenceComponent>(target)) {
      return NumberPair(noise->baseFrequencyX, noise->baseFrequencyY);
    }
  }
  if (name == "order") {
    if (const auto* convolution = registry.try_get<FEConvolveMatrixComponent>(target)) {
      return NumberPair(convolution->orderX, convolution->orderY);
    }
  }
  return std::nullopt;
}

std::optional<std::string> FilterIntegerBaseValue(Registry& registry, Entity target,
                                                  std::string_view name,
                                                  const AnimationValueContext& context) {
  if (name == "numOctaves") {
    if (const auto* noise = registry.try_get<FETurbulenceComponent>(target)) {
      return SerializeValue(noise->numOctaves, context);
    }
  }
  const auto* convolution = registry.try_get<FEConvolveMatrixComponent>(target);
  if (!convolution) {
    return std::nullopt;
  }
  if (name == "targetX") {
    return SerializeValue(convolution->targetX.value_or(convolution->orderX / 2), context);
  }
  if (name == "targetY") {
    return SerializeValue(convolution->targetY.value_or(convolution->orderY / 2), context);
  }
  return std::nullopt;
}

std::optional<std::string> FilterNumberBaseValue(Registry& registry, Entity target,
                                                 std::string_view name,
                                                 const AnimationValueContext& context,
                                                 AnimationResourceBudget& budget) {
  if (auto pair = FilterPairBaseValue(registry, target, name)) {
    return pair;
  }
  if (auto integer = FilterIntegerBaseValue(registry, target, name, context)) {
    return integer;
  }
  if (name == "divisor") {
    if (const auto* convolution = registry.try_get<FEConvolveMatrixComponent>(target);
        convolution && !convolution->divisor) {
      if (!budget.reserveGeometryWork(convolution->kernelMatrix.size())) {
        return std::string{};
      }
      double sum = 0;
      for (double value : convolution->kernelMatrix) {
        sum += value;
      }
      return SerializeValue(sum == 0 ? 1 : sum, context);
    }
  }
  std::optional<std::string> result;
  VisitAnimationNumberAttribute(registry, target, name, [&](const auto& component, auto field) {
    const auto& value = component.*field;
    if constexpr (requires { value.has_value(); }) {
      result = value ? SerializeValue(*value, context) : std::optional(std::string{});
    } else {
      result = SerializeValue(value, context);
    }
  });
  return result;
}

std::optional<std::string> StyleValue(const PropertyRegistry& properties,
                                      std::string_view attribute,
                                      const AnimationValueContext& context,
                                      AnimationResourceBudget& budget) {
  std::optional<std::string> result;
  const auto all = properties.allProperties();
  PropertyRegistry::forEachProperty<0, PropertyRegistry::numProperties()>([&](auto index) {
    const auto& property = std::get<index>(all);
    if (property.name != attribute) {
      return;
    }
    if (const auto* value = property.getStoredValue()) {
      result = SerializeValue(*value, context, &budget);
    } else if (const auto value = property.get()) {
      result = SerializeValue(*value, context, &budget);
    } else {
      using Value = std::remove_cvref_t<decltype(*property.getStoredValue())>;
      if constexpr (std::is_same_v<Value, StrokeDasharray> || std::is_same_v<Value, Reference>) {
        result = "none";
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
  result.referenceValue = attribute == "href" || attribute == "xlink:href" || attribute == "in" ||
                          attribute == "in2" || attribute == "result";
  result.numberPair = attribute == "stdDeviation" || attribute == "radius" ||
                      attribute == "baseFrequency" || attribute == "order";
  result.integerValue = attribute == "numOctaves" || attribute == "targetX" ||
                        attribute == "targetY" || attribute == "order";
  result.viewBox = LayoutSystem().getViewBox(EntityHandle(registry, target));
  if (ResourceUsesBoundingBoxUnits(registry, target, budget)) {
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
  if (auto value = LayoutBaseValue(registry, target, attribute, context)) {
    return *value;
  }
  if (auto value = GradientBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto value = PatternBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto value = FilterRegionBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto* sized = registry.try_get<SizedElementComponent>(target)) {
    if (auto value = PropertyValue(sized->properties, attribute, context, true)) {
      return *value;
    }
  }
  if (auto value = FilterReferenceBaseValue(registry, target, attribute, budget)) {
    return *value;
  }
  if (auto value = FilterNumberBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (attribute == "href" || attribute == "xlink:href") {
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
    if (const auto value = StyleValue(*style->properties, attribute, context, budget)) {
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
