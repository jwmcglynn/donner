#include "donner/svg/components/animation/AnimationBaseValue.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationNumericAttributes.h"
#include "donner/svg/components/animation/AnimationPathValue.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {
namespace {

std::optional<std::string> SerializeReference(const Reference& reference,
                                              AnimationResourceBudget* budget) {
  const std::string_view href(reference.href);
  if (!budget || !reference.hasSafeRepresentation()) {
    return std::nullopt;
  }
  if (href.size() > (AnimationResourceBudget::kMaximumSourceBytes - 32) / 4 ||
      !budget->reserveSourceBytes(href.size() * 4 + 32)) {
    std::ignore = budget->reserveSourceBytes(AnimationResourceBudget::kMaximumSourceBytes + 1);
    return std::string{};
  }
  return SerializeAnimationUrl(href);
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
  out.precision(std::numeric_limits<double>::max_digits10);
  out << value;
  return out.str();
}

template <typename T>
std::optional<std::string> SerializeValue(const T& value, const AnimationValueContext& context,
                                          AnimationResourceBudget* budget = nullptr) {
  if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, Lengthd> || std::is_enum_v<T>) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
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
                                            std::optional<Value> Component::* field,
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
  output.precision(std::numeric_limits<double>::max_digits10);
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

template <typename Values>
std::string SerializeTextPositionBase(const Values& values, AnimationResourceBudget& budget) {
  if (values.empty()) {
    return {};
  }
  if (values.size() > AnimationResourceBudget::kMaximumNumbersPerValue ||
      !budget.reserveSourceBytes(values.size() * 64)) {
    return {};
  }
  std::ostringstream output;
  output.precision(std::numeric_limits<double>::max_digits10);
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      output << ' ';
    }
    output << value;
    first = false;
  }
  return output.str();
}

template <typename Field>
auto InheritedTextPositionValues(Registry& registry, Entity target, Field field,
                                 AnimationResourceBudget& budget) {
  using Values = std::remove_reference_t<decltype(std::declval<TextPositioningComponent>().*field)>;
  Entity current = target;
  while (current != entt::null && registry.valid(current)) {
    if (!budget.reserveReferenceWork(1)) {
      break;
    }
    if (const auto* component = registry.try_get<TextPositioningComponent>(current);
        component && !(component->*field).empty()) {
      const auto& values = component->*field;
      return current == target || values.size() == 1 ? &values
                                                     : static_cast<const Values*>(nullptr);
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    current = tree ? tree->parent() : entt::null;
  }
  return static_cast<const Values*>(nullptr);
}

std::optional<std::string> TextPositionBaseValue(Registry& registry, Entity target,
                                                 std::string_view name,
                                                 const AnimationValueContext& context,
                                                 AnimationResourceBudget& budget) {
  const auto* text = registry.try_get<TextPositioningComponent>(target);
  if (!text) {
    return std::nullopt;
  }
  if (name == "rotate") {
    const auto* values = InheritedTextPositionValues(
        registry, target, &TextPositioningComponent::rotateDegrees, budget);
    return values ? SerializeTextPositionBase(*values, budget) : std::string{};
  }
  using Field = SmallVector<Lengthd, 1> TextPositioningComponent::*;
  const std::pair<std::string_view, Field> fields[] = {{"x", &TextPositioningComponent::x},
                                                       {"y", &TextPositioningComponent::y},
                                                       {"dx", &TextPositioningComponent::dx},
                                                       {"dy", &TextPositioningComponent::dy}};
  for (const auto& [attribute, field] : fields) {
    if (name == attribute) {
      const auto* values = InheritedTextPositionValues(registry, target, field, budget);
      if (!values) {
        return std::string{};
      }
      if (name == "x" || name == "y") {
        return SerializeValue(values->front(), context);
      }
      return SerializeTextPositionBase(*values, budget);
    }
  }
  return std::nullopt;
}

std::optional<std::string> TypedPresentationBaseValue(Registry& registry, Entity target,
                                                      std::string_view name,
                                                      const AnimationValueContext& context) {
  if (const auto* length = registry.try_get<PathLengthComponent>(target);
      length && name == "pathLength") {
    return SerializeValue(length->value, context);
  }
  if (const auto* box = registry.try_get<ViewBoxComponent>(target);
      box && box->viewBox && name == "viewBox") {
    std::ostringstream output;
    output.precision(std::numeric_limits<double>::max_digits10);
    output << box->viewBox->topLeft.x << ' ' << box->viewBox->topLeft.y << ' '
           << box->viewBox->width() << ' ' << box->viewBox->height();
    return output.str();
  }
  if (const auto* text = registry.try_get<TextComponent>(target);
      text && text->textLength && name == "textLength") {
    return SerializeValue(*text->textLength, context);
  }
  return std::nullopt;
}

std::optional<std::string> MarkerOrientBaseValue(Registry& registry, Entity target,
                                                 std::string_view name,
                                                 const AnimationValueContext& context) {
  const auto* marker = registry.try_get<MarkerComponent>(target);
  if (!marker || name != "orient") {
    return std::nullopt;
  }
  if (marker->orient.type() == MarkerOrient::Type::Auto) {
    return "auto";
  }
  if (marker->orient.type() == MarkerOrient::Type::AutoStartReverse) {
    return "auto-start-reverse";
  }
  return SerializeValue(marker->orient.computeAngleRadians(Vector2d(1, 0)), context).value() +
         "rad";
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

enum class AttributeTrait : uint16_t {
  None = 0,
  Reference = 1 << 0,
  NumberPair = 1 << 1,
  Integer = 1 << 2,
  Scalar = 1 << 3,
  MarkerAngle = 1 << 4,
  StopPercentage = 1 << 5,
  ColorMatrixList = 1 << 6,
  ConvolveList = 1 << 7,
  FunctionList = 1 << 8,
};

constexpr AttributeTrait operator|(AttributeTrait lhs, AttributeTrait rhs) {
  return static_cast<AttributeTrait>(static_cast<uint16_t>(lhs) | static_cast<uint16_t>(rhs));
}

struct AnimationAttributeTraits {
  std::string_view name;
  Lengthd::Extent extent = Lengthd::Extent::Mixed;
  AttributeTrait flags = AttributeTrait::None;
};

constexpr bool HasTrait(const AnimationAttributeTraits& traits, AttributeTrait flag) {
  return (static_cast<uint16_t>(traits.flags) & static_cast<uint16_t>(flag)) != 0;
}

const AnimationAttributeTraits& LookupAnimationAttributeTraits(std::string_view name) {
  static constexpr std::array kTraits = {
      AnimationAttributeTraits{"x", Lengthd::Extent::X},
      AnimationAttributeTraits{"dx", Lengthd::Extent::X},
      AnimationAttributeTraits{"x1", Lengthd::Extent::X},
      AnimationAttributeTraits{"x2", Lengthd::Extent::X},
      AnimationAttributeTraits{"cx", Lengthd::Extent::X},
      AnimationAttributeTraits{"rx", Lengthd::Extent::X},
      AnimationAttributeTraits{"width", Lengthd::Extent::X},
      AnimationAttributeTraits{"fx", Lengthd::Extent::X},
      AnimationAttributeTraits{"refX", Lengthd::Extent::X},
      AnimationAttributeTraits{"markerWidth", Lengthd::Extent::X},
      AnimationAttributeTraits{"y", Lengthd::Extent::Y},
      AnimationAttributeTraits{"dy", Lengthd::Extent::Y},
      AnimationAttributeTraits{"y1", Lengthd::Extent::Y},
      AnimationAttributeTraits{"y2", Lengthd::Extent::Y},
      AnimationAttributeTraits{"cy", Lengthd::Extent::Y},
      AnimationAttributeTraits{"ry", Lengthd::Extent::Y},
      AnimationAttributeTraits{"height", Lengthd::Extent::Y},
      AnimationAttributeTraits{"fy", Lengthd::Extent::Y},
      AnimationAttributeTraits{"refY", Lengthd::Extent::Y},
      AnimationAttributeTraits{"markerHeight", Lengthd::Extent::Y},
      AnimationAttributeTraits{"href", Lengthd::Extent::Mixed, AttributeTrait::Reference},
      AnimationAttributeTraits{"xlink:href", Lengthd::Extent::Mixed, AttributeTrait::Reference},
      AnimationAttributeTraits{"in", Lengthd::Extent::Mixed, AttributeTrait::Reference},
      AnimationAttributeTraits{"in2", Lengthd::Extent::Mixed, AttributeTrait::Reference},
      AnimationAttributeTraits{"result", Lengthd::Extent::Mixed, AttributeTrait::Reference},
      AnimationAttributeTraits{"stdDeviation", Lengthd::Extent::Mixed, AttributeTrait::NumberPair},
      AnimationAttributeTraits{"radius", Lengthd::Extent::Mixed, AttributeTrait::NumberPair},
      AnimationAttributeTraits{"baseFrequency", Lengthd::Extent::Mixed, AttributeTrait::NumberPair},
      AnimationAttributeTraits{"order", Lengthd::Extent::Mixed,
                               AttributeTrait::NumberPair | AttributeTrait::Integer},
      AnimationAttributeTraits{"numOctaves", Lengthd::Extent::Mixed, AttributeTrait::Integer},
      AnimationAttributeTraits{"targetX", Lengthd::Extent::Mixed, AttributeTrait::Integer},
      AnimationAttributeTraits{"targetY", Lengthd::Extent::Mixed, AttributeTrait::Integer},
      AnimationAttributeTraits{"pathLength", Lengthd::Extent::Mixed, AttributeTrait::Scalar},
      AnimationAttributeTraits{"orient", Lengthd::Extent::Mixed, AttributeTrait::MarkerAngle},
      AnimationAttributeTraits{"offset", Lengthd::Extent::Mixed, AttributeTrait::StopPercentage},
      AnimationAttributeTraits{"values", Lengthd::Extent::Mixed, AttributeTrait::ColorMatrixList},
      AnimationAttributeTraits{"kernelMatrix", Lengthd::Extent::Mixed,
                               AttributeTrait::ConvolveList},
      AnimationAttributeTraits{"tableValues", Lengthd::Extent::Mixed, AttributeTrait::FunctionList},
  };
  static constexpr AnimationAttributeTraits kDefault{""};
  const auto found = std::find_if(kTraits.begin(), kTraits.end(),
                                  [name](const auto& item) { return item.name == name; });
  return found == kTraits.end() ? kDefault : *found;
}

void ConfigureNumericContext(Registry& registry, Entity target, std::string_view name,
                             const AnimationAttributeTraits& traits,
                             AnimationValueContext& context) {
  context.percentageNumbers =
      HasTrait(traits, AttributeTrait::StopPercentage) && registry.all_of<StopComponent>(target);
  const bool scalar =
      context.percentageNumbers || HasTrait(traits, AttributeTrait::Scalar) ||
      context.integerValue ||
      VisitAnimationNumberAttribute(registry, target, name, [](const auto&, auto) {});
  if (scalar || context.numberPair) {
    context.maximumNumbers = context.numberPair ? 2 : 1;
  }
  if ((HasTrait(traits, AttributeTrait::ColorMatrixList) &&
       registry.all_of<FEColorMatrixComponent>(target)) ||
      (HasTrait(traits, AttributeTrait::ConvolveList) &&
       registry.all_of<FEConvolveMatrixComponent>(target)) ||
      (HasTrait(traits, AttributeTrait::FunctionList) &&
       registry.all_of<FEFuncComponent>(target))) {
    context.maximumNumbers = AnimationResourceBudget::kMaximumNumbersPerValue;
  }
}

/// Share readers for repeated property types across the expanded registry tuple.
template <typename PropertyType>
[[gnu::noinline]] void ReadStyleProperty(const PropertyType& property,
                                         const AnimationValueContext& context,
                                         AnimationResourceBudget& budget,
                                         std::optional<std::string>& result) {
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
    ReadStyleProperty(property, context, budget, result);
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

void ConfigureAnimationStyleContext(Registry& registry, Entity target, Entity root,
                                    AnimationValueContext& context) {
  if (const auto* style = registry.try_get<ComputedStyleComponent>(target);
      style && style->properties) {
    context.linearRgb = style->properties->colorInterpolation.getOr(ColorInterpolation::SRGB) ==
                        ColorInterpolation::LinearRGB;
    if (const auto color = style->properties->color.get()) {
      context.currentColor = color->resolve(context.currentColor, 1);
    }
    if (const auto font = style->properties->fontSize.get()) {
      context.fontMetrics.fontSize = font->toPixels(context.viewBox, context.fontMetrics);
    }
  }
  if (const auto* style = registry.try_get<ComputedStyleComponent>(root);
      style && style->properties) {
    if (const auto font = style->properties->fontSize.get()) {
      context.fontMetrics.rootFontSize = font->toPixels(context.viewBox, context.fontMetrics);
    }
  }
}

void ConfigureValueKinds(Registry& registry, Entity target, const AnimationAttributeTraits& traits,
                         AnimationValueContext& context) {
  context.angleValue =
      HasTrait(traits, AttributeTrait::MarkerAngle) && registry.all_of<MarkerComponent>(target);
  context.referenceValue = HasTrait(traits, AttributeTrait::Reference);
  context.numberPair = HasTrait(traits, AttributeTrait::NumberPair);
  context.integerValue = HasTrait(traits, AttributeTrait::Integer);
}

AnimationValueContext AnimationContextFor(Registry& registry, Entity target,
                                          std::string_view attribute,
                                          AnimationResourceBudget& budget) {
  AnimationValueContext result;
  const AnimationAttributeTraits& traits = LookupAnimationAttributeTraits(attribute);
  ConfigureValueKinds(registry, target, traits, result);
  ConfigureNumericContext(registry, target, attribute, traits, result);
  result.viewBox = LayoutSystem().getViewBox(EntityHandle(registry, target));
  if (ResourceUsesBoundingBoxUnits(registry, target, budget)) {
    result.viewBox = Box2d{{0, 0}, {1, 1}};
  }
  result.extent = traits.extent;
  const auto& document = registry.ctx().get<SVGDocumentContext>();
  if (document.canvasSize) {
    result.fontMetrics.viewportSize = Vector2d(*document.canvasSize);
  }
  ConfigureAnimationStyleContext(registry, target, document.rootEntity, result);
  return result;
}

static std::optional<RcString> AnimationPathData(Registry& registry, Entity target,
                                                 AnimationResourceBudget& budget,
                                                 bool includeAnimationOverride) {
  Property<RcString> data("d");
  if (const auto* path = registry.try_get<PathComponent>(target)) {
    data = path->d;
  }
  if (includeAnimationOverride) {
    if (const auto* animated = registry.try_get<AnimatedValuesComponent>(target);
        animated && animated->overrides.contains("d")) {
      const auto& value = registry.get<AnimatedValuesComponent>(target).overrides.at("d");
      if (value.size() > AnimationResourceBudget::kMaximumPathValueBytes ||
          !budget.reserveSourceBytes(value.size())) {
        return std::nullopt;
      }
      data.substitute(RcString(value));
      data.specificity = css::Specificity::Animation();
    }
  }
  if (const auto* style = registry.try_get<ComputedStyleComponent>(target);
      style && style->properties) {
    const auto it = style->properties->unparsedProperties.find("d");
    if (it != style->properties->unparsedProperties.end()) {
      if (!budget.reserveSourceBytes(style->properties->complexPropertyBytes())) {
        return std::nullopt;
      }
      const auto params =
          parser::PropertyParseFnParams::Create(it->second.declaration, it->second.specificity,
                                                parser::PropertyParseBehavior::AllowUserUnits);
      if (parser::Parse(
              params,
              [](const auto& params) { return parser::ParsePathDataProperty(params.components()); },
              &data)) {
        return std::nullopt;
      }
    }
  }
  return data.get();
}

std::optional<RcString> CascadedAnimationPathData(Registry& registry, Entity target,
                                                  AnimationResourceBudget& budget) {
  return AnimationPathData(registry, target, budget, true);
}

static std::optional<std::string> AnimationBaseOrUnderlyingValue(
    Registry& registry, Entity target, std::string_view attribute,
    const AnimationValueContext& context, AnimationResourceBudget& budget,
    bool includeAnimationOverride) {
  if (includeAnimationOverride) {
    const auto* animated = registry.try_get<AnimatedValuesComponent>(target);
    if (animated) {
      if (const auto it = animated->overrides.find(std::string(attribute));
          it != animated->overrides.end()) {
        return budget.reserveSourceBytes(it->second.size()) ? it->second : std::string{};
      }
    }
  }

  if (attribute == "d") {
    if (const auto* path = registry.try_get<PathComponent>(target); path && path->splineOverride) {
      return SerializeAnimationPathGeometry(*path->splineOverride, budget);
    }
    const auto data = AnimationPathData(registry, target, budget, includeAnimationOverride);
    if (!data || data->size() > AnimationResourceBudget::kMaximumPathValueBytes ||
        !budget.reserveSourceBytes(data->size())) {
      return std::nullopt;
    }
    return data->str();
  }
  if (attribute == "offset") {
    if (const auto* stop = registry.try_get<StopComponent>(target)) {
      return SerializeValue(stop->properties.offset, context).value();
    }
  }
  if (auto value = TextPositionBaseValue(registry, target, attribute, context, budget)) {
    return *value;
  }
  if (auto value = TypedPresentationBaseValue(registry, target, attribute, context)) {
    return *value;
  }
  if (auto value = MarkerOrientBaseValue(registry, target, attribute, context)) {
    return *value;
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
    return std::string{};
  }
  if (attribute == "fill" || attribute == "color") {
    return "black";
  }
  if (attribute == "stroke") {
    return "none";
  }
  return "0";
}

std::optional<std::string> UnderlyingAnimationValue(Registry& registry, Entity target,
                                                    std::string_view attribute,
                                                    const AnimationValueContext& context,
                                                    AnimationResourceBudget& budget) {
  return AnimationBaseOrUnderlyingValue(registry, target, attribute, context, budget, true);
}

std::optional<std::string> BaseAnimationValue(Registry& registry, Entity target,
                                              std::string_view attribute,
                                              const AnimationValueContext& context,
                                              AnimationResourceBudget& budget) {
  return AnimationBaseOrUnderlyingValue(registry, target, attribute, context, budget, false);
}

std::shared_ptr<const ContextualLengthRecipe> ContextualLengthFor(Registry& registry, Entity target,
                                                                  std::string_view attribute) {
  if (const auto* shadow = registry.try_get<ShadowEntityComponent>(target)) {
    target = shadow->lightEntity;
  }
  const auto* values = registry.try_get<AnimatedValuesComponent>(target);
  if (!values) {
    return nullptr;
  }
  const auto found = values->contextualLengths.find(std::string(attribute));
  return found == values->contextualLengths.end() ? nullptr : found->second;
}

std::optional<Lengthd> ResolveContextualLength(Registry& registry, Entity target,
                                               std::string_view attribute, Lengthd underlying,
                                               const AnimationValueContext& context) {
  const auto recipe = ContextualLengthFor(registry, target, attribute);
  auto* budget = registry.ctx().find<AnimationResourceBudget>();
  if (!recipe || !budget || !budget->reserveContextualWork(recipe->evaluationWork)) {
    return std::nullopt;
  }
  return SampleContextualLengthRecipe(*recipe, underlying, context);
}

}  // namespace donner::svg::components
