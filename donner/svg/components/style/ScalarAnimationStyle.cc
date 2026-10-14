#include "donner/svg/components/style/ScalarAnimationStyle.h"

#include <cmath>
#include <type_traits>

#include "donner/base/StringUtils.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {
namespace {

struct ScalarContext {
  double parent = 1;
  double initial = 1;
  bool inherited = false;
  bool percentages = true;
};

std::optional<double> ResolveScalarOperand(std::string_view value, const ScalarContext& context) {
  value = StringUtils::TrimWhitespace(value);
  if (StringUtils::EqualsLowercase(value, std::string_view("initial"))) {
    return context.initial;
  }
  if (StringUtils::EqualsLowercase(value, std::string_view("inherit"))) {
    return context.parent;
  }
  if (StringUtils::EqualsLowercase(value, std::string_view("unset"))) {
    return context.inherited ? context.parent : context.initial;
  }
  const auto numbers = ParseAnimationLengths(value);
  if (!numbers || numbers->size() != 1) {
    return std::nullopt;
  }
  const auto& number = numbers->front();
  if (number.unit == Lengthd::Unit::None) {
    return number.value;
  }
  return context.percentages && number.unit == Lengthd::Unit::Percent
             ? std::optional(number.value / 100)
             : std::nullopt;
}

bool NormalizeScalarOperand(std::optional<std::string>& value, const ScalarContext& context) {
  if (!value) {
    return true;
  }
  const auto number = ResolveScalarOperand(*value, context);
  if (!number || !std::isfinite(*number)) {
    return false;
  }
  value = Lengthd(*number).toRcString().str();
  return true;
}

bool NormalizeScalarDeclaration(AnimateValueComponent& declaration, const ScalarContext& context) {
  if (declaration.values.empty()) {
    if (declaration.to) {
      return NormalizeScalarOperand(declaration.from, context) &&
             NormalizeScalarOperand(declaration.to, context);
    }
    return NormalizeScalarOperand(declaration.from, context) &&
           NormalizeScalarOperand(declaration.by, context);
  }
  for (auto& value : declaration.values) {
    const auto number = ResolveScalarOperand(value, context);
    if (!number || !std::isfinite(*number)) {
      return false;
    }
    value = Lengthd(*number).toRcString().str();
  }
  return true;
}

std::optional<double> SampleScalarLayer(const ContextualLengthSample& source, double underlying,
                                        const ScalarContext& context) {
  ContextualLengthSample sample = source;
  const bool valid = std::visit(
      [&](auto& declaration) {
        if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>, AnimateValueComponent>) {
          return NormalizeScalarDeclaration(declaration, context);
        } else {
          return NormalizeScalarOperand(declaration.to, context);
        }
      },
      sample.declaration);
  if (!valid) {
    return std::nullopt;
  }
  AnimationValueContext numericContext;
  numericContext.maximumNumbers = 1;
  const auto value = SampleAnimationValue(
      sample.progress, sample.iteration, std::get_if<SetAnimationComponent>(&sample.declaration),
      std::get_if<AnimateValueComponent>(&sample.declaration), nullptr,
      Lengthd(underlying).toRcString().str(), numericContext);
  return value ? ResolveScalarOperand(value->second, context) : std::nullopt;
}

std::optional<double> SampleScalarStyle(Registry& registry, Entity source, std::string_view name,
                                        double underlying, const ScalarContext& context) {
  const auto* enabled = registry.ctx().find<AnimationStyleContext>();
  const auto* values = registry.try_get<AnimatedValuesComponent>(source);
  if (!enabled || !enabled->enabled || !values) {
    return std::nullopt;
  }
  const auto recipe = values->contextualLengths.find(std::string(name));
  if (recipe == values->contextualLengths.end() ||
      !registry.ctx().get<AnimationResourceBudget>().reserveContextualWork(
          recipe->second->evaluationWork)) {
    return std::nullopt;
  }
  std::optional<double> result;
  for (const auto& sample : recipe->second->samples) {
    if (const auto value = SampleScalarLayer(sample, underlying, context);
        value && std::isfinite(*value)) {
      underlying = *value;
      result = *value;
    }
  }
  return result;
}

template <typename PropertyType>
ScalarContext ContextForScalar(const PropertyType& property, double parent) {
  return {.parent = parent,
          .initial = property.getInitialFn().value_or(0),
          .inherited = PropertyType::kCascadeMode != PropertyCascade::None,
          .percentages = property.name != "stroke-miterlimit"};
}

template <typename PropertyType>
double BaseScalar(const PropertyType& property, const ScalarContext& context) {
  switch (property.state) {
    case PropertyState::NotSet:
    case PropertyState::ExplicitUnset: return context.inherited ? context.parent : context.initial;
    case PropertyState::Inherit: return context.parent;
    case PropertyState::ExplicitInitial: return context.initial;
    default: return property.get().value_or(context.initial);
  }
}

template <typename PropertyType>
void ApplyRegisteredScalar(Registry& registry, Entity source, PropertyRegistry& properties,
                           const PropertyType& property, double parent) {
  const auto context = ContextForScalar(property, parent);
  if (const auto value = SampleScalarStyle(registry, source, property.name,
                                           BaseScalar(property, context), context)) {
    std::ignore =
        properties.parsePresentationAttribute(property.name, Lengthd(*value).toRcString().str(),
                                              EntityHandle(), css::Specificity::Animation());
  }
}

Property<double> ResourceOpacityBase(Registry& registry, Entity source, bool flood,
                                     const PropertyRegistry& properties,
                                     StyleResourceBudget& budget, ParseWarningSink& warnings) {
  Property<double> result{flood ? "flood-opacity" : "stop-opacity",
                          []() -> std::optional<double> { return 1; }};
  if (flood) {
    if (const auto* component = registry.try_get<FEFloodComponent>(source)) {
      result = component->floodOpacity;
    } else if (const auto* shadow = registry.try_get<FEDropShadowComponent>(source)) {
      result = shadow->floodOpacity;
    }
  } else if (const auto* stop = registry.try_get<StopComponent>(source)) {
    result = stop->properties.stopOpacity;
  }
  const auto declared = properties.unparsedProperties.find(RcString(result.name));
  if (declared == properties.unparsedProperties.end()) {
    return result;
  }
  const auto& declaration = declared->second.declaration;
  if (!budget.reserveDeclarationApplication(declaration.values.size(),
                                            declaration.sourceByteSize)) {
    return result;
  }
  const auto params = parser::PropertyParseFnParams::Create(
      declaration, declared->second.specificity, parser::PropertyParseBehavior::AllowUserUnits);
  if (auto error = Parse(
          params,
          [](const parser::PropertyParseFnParams& input) {
            return parser::ParseAlphaValue(input.components());
          },
          &result)) {
    warnings.add(std::move(*error));
  }
  return result;
}

double ApplyResourceScalar(Registry& registry, Entity source, Property<double>& property,
                           double parent) {
  const auto context = ContextForScalar(property, parent);
  if (const auto value = SampleScalarStyle(registry, source, property.name,
                                           BaseScalar(property, context), context)) {
    const std::string text = Lengthd(*value).toRcString().str();
    auto params = parser::PropertyParseFnParams::CreateForAttribute(text);
    params.specificity = css::Specificity::Animation();
    std::ignore = Parse(
        params,
        [](const parser::PropertyParseFnParams& input) {
          return parser::ParseAlphaValue(input.components());
        },
        &property);
  }
  return BaseScalar(property, context);
}

}  // namespace

ComputedResourceOpacities ApplyScalarAnimationStyles(Registry& registry, Entity source,
                                                     const ComputedStyleComponent* parent,
                                                     PropertyRegistry& properties,
                                                     StyleResourceBudget& budget,
                                                     ParseWarningSink& warnings) {
  static const PropertyRegistry defaults;
  const PropertyRegistry& inherited = parent && parent->properties ? *parent->properties : defaults;
  ApplyRegisteredScalar(registry, source, properties, properties.opacity,
                        inherited.opacity.get().value());
  ApplyRegisteredScalar(registry, source, properties, properties.fillOpacity,
                        inherited.fillOpacity.get().value());
  ApplyRegisteredScalar(registry, source, properties, properties.strokeOpacity,
                        inherited.strokeOpacity.get().value());
  ApplyRegisteredScalar(registry, source, properties, properties.strokeMiterlimit,
                        inherited.strokeMiterlimit.get().value());
  auto stop = ResourceOpacityBase(registry, source, false, properties, budget, warnings);
  auto flood = ResourceOpacityBase(registry, source, true, properties, budget, warnings);
  const auto parentValues = parent ? parent->resourceOpacities.value_or(ComputedResourceOpacities())
                                   : ComputedResourceOpacities();
  const double stopValue = ApplyResourceScalar(registry, source, stop, parentValues.stopOpacity);
  const double floodValue = ApplyResourceScalar(registry, source, flood, parentValues.floodOpacity);
  return {.stopOpacity = stopValue,
          .floodOpacity = floodValue,
          .stopSpecificity = stop.specificity,
          .floodSpecificity = flood.specificity};
}

}  // namespace donner::svg::components
