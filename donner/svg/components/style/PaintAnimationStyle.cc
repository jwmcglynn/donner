#include "donner/svg/components/style/PaintAnimationStyle.h"

#include <tuple>
#include <type_traits>

#include "donner/base/StringUtils.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/StopComponent.h"

namespace donner::svg::components {
namespace {

std::string ColorText(const css::Color& color) {
  return color.isCurrentColor() ? "currentColor" : color.asRGBA().toHexString();
}

std::optional<std::string> SerializePaintValue(const css::Color& color,
                                               AnimationResourceBudget& budget) {
  return budget.reserveContextualWork(32) ? std::optional(ColorText(color)) : std::nullopt;
}

std::optional<std::string> SerializePaintValue(const PaintServer& paint,
                                               AnimationResourceBudget& budget) {
  if (paint.is<PaintServer::Solid>()) {
    return SerializePaintValue(paint.get<PaintServer::Solid>().color, budget);
  }
  if (!budget.reserveContextualWork(32)) {
    return std::nullopt;
  }
  if (paint.is<PaintServer::None>()) {
    return "none";
  }
  if (paint.is<PaintServer::ContextFill>()) {
    return "context-fill";
  }
  if (paint.is<PaintServer::ContextStroke>()) {
    return "context-stroke";
  }
  const auto& reference = paint.get<PaintServer::ElementReference>();
  const std::string_view href(reference.reference.href);
  if (!reference.reference.hasSafeRepresentation() ||
      href.size() > AnimationResourceBudget::kMaximumContextualWork / 4 ||
      !budget.reserveContextualWork(href.size() * 4)) {
    return std::nullopt;
  }
  auto result = SerializeAnimationUrl(href);
  if (result && reference.fallback) {
    *result += " " + ColorText(*reference.fallback);
  }
  return result;
}

template <typename P>
bool UsesParent(const P& property) {
  return property.state == PropertyState::Inherit ||
         (P::kCascadeMode != PropertyCascade::None &&
          (property.state == PropertyState::NotSet ||
           property.state == PropertyState::ExplicitUnset));
}

template <typename P>
auto BaseValue(const P& property, const P& parent) {
  return UsesParent(property) ? parent.get() : property.get();
}

template <typename T>
std::optional<std::string> SerializeBase(const std::optional<T>& value, bool inherited,
                                         AnimationResourceBudget& budget) {
  if (!value) {
    return std::string{};
  }
  if constexpr (std::is_same_v<T, PaintServer>) {
    // Preserve the inherited URL's owner; serializing it would make it a local reference.
    if (inherited && !value->template is<PaintServer::Solid>()) {
      return "inherit";
    }
  }
  return SerializePaintValue(*value, budget);
}

struct PaintKeywords {
  std::string initial;
  std::string parent;
  bool inherited;
  bool currentColorIsParent;
};

bool NormalizeOperand(std::string& text, const PaintKeywords& keywords,
                      AnimationResourceBudget& budget) {
  const auto input = StringUtils::TrimWhitespace(std::string_view(text));
  const std::string* replacement = nullptr;
  if (StringUtils::EqualsLowercase(input, std::string_view("initial"))) {
    replacement = &keywords.initial;
  } else if (StringUtils::EqualsLowercase(input, std::string_view("inherit")) ||
             (keywords.currentColorIsParent &&
              StringUtils::EqualsLowercase(input, std::string_view("currentcolor")))) {
    replacement = &keywords.parent;
  } else if (StringUtils::EqualsLowercase(input, std::string_view("unset"))) {
    replacement = keywords.inherited ? &keywords.parent : &keywords.initial;
  }
  if (!replacement) {
    return true;
  }
  if (!budget.reserveContextualWork(replacement->size())) {
    return false;
  }
  text = *replacement;
  return true;
}

bool NormalizeOperand(std::optional<std::string>& text, const PaintKeywords& keywords,
                      AnimationResourceBudget& budget) {
  return !text || NormalizeOperand(*text, keywords, budget);
}

bool NormalizeDeclaration(ContextualLengthSample& sample, const PaintKeywords& keywords,
                          AnimationResourceBudget& budget) {
  return std::visit(
      [&](auto& declaration) {
        if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>, AnimateValueComponent>) {
          if (!declaration.values.empty()) {
            for (auto& value : declaration.values) {
              if (!NormalizeOperand(value, keywords, budget)) {
                return false;
              }
            }
            return true;
          }
          return NormalizeOperand(declaration.from, keywords, budget) &&
                 (declaration.to ? NormalizeOperand(declaration.to, keywords, budget)
                                 : NormalizeOperand(declaration.by, keywords, budget));
        } else {
          return NormalizeOperand(declaration.to, keywords, budget);
        }
      },
      sample.declaration);
}

bool NeedsUnderlying(const ContextualLengthSample& sample) {
  const auto* value = std::get_if<AnimateValueComponent>(&sample.declaration);
  return value && (value->additive || (value->values.empty() && !value->from));
}

bool ValidPaintValue(PropertyRegistry& validator, std::string_view name, std::string_view value) {
  const auto parsed = validator.parsePresentationAttribute(name, value, EntityHandle(),
                                                           css::Specificity::Animation());
  return parsed.hasResult() && parsed.result();
}

bool ValidDeclaration(const ContextualLengthSample& sample, std::string_view name,
                      const std::string& underlying, PropertyRegistry& validator) {
  const auto valid = [&](std::string_view value) {
    return ValidPaintValue(validator, name, value);
  };
  return std::visit(
      [&](const auto& declaration) {
        if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>, AnimateValueComponent>) {
          if (!declaration.values.empty()) {
            for (const auto& value : declaration.values) {
              if (!valid(value)) {
                return false;
              }
            }
            return true;
          }
          if (declaration.to) {
            return valid(declaration.from ? *declaration.from : underlying) &&
                   valid(*declaration.to);
          }
          return declaration.by && valid(*declaration.by) &&
                 (!declaration.from || valid(*declaration.from));
        } else {
          return declaration.to && valid(*declaration.to);
        }
      },
      sample.declaration);
}

template <typename BaseProvider>
std::optional<std::string> SamplePaintStack(const ContextualLengthRecipe& recipe,
                                            std::string_view name, const PaintKeywords& keywords,
                                            const AnimationValueContext& context,
                                            AnimationResourceBudget& budget, BaseProvider base) {
  PropertyRegistry validator;
  using PaintState = std::optional<std::string>;
  std::optional<PaintState> cachedBase;
  bool refused = false;
  const std::string empty;
  const auto sampled = SampleContextualAnimationRecipe(
      recipe, PaintState{},
      [&](const ContextualLengthSample& source,
          PaintState underlying) -> std::optional<PaintState> {
        if (refused) {
          return std::nullopt;
        }
        if (NeedsUnderlying(source)) {
          if (!underlying) {
            if (!cachedBase) {
              cachedBase = base();
            }
            underlying = *cachedBase;
            if (!underlying) {
              refused = true;
              return std::nullopt;
            }
          }
          if (underlying->size() > AnimationResourceBudget::kMaximumContextualWork / 16 ||
              !budget.reserveContextualWork(underlying->size() * 16)) {
            refused = true;
            return std::nullopt;
          }
        }
        ContextualLengthSample sample = source;
        if (!NormalizeDeclaration(sample, keywords, budget)) {
          refused = true;
          return std::nullopt;
        }
        if (!ValidDeclaration(sample, name, underlying ? *underlying : empty, validator)) {
          return std::nullopt;
        }
        const auto value =
            SampleAnimationValue(sample.progress, sample.iteration,
                                 std::get_if<SetAnimationComponent>(&sample.declaration),
                                 std::get_if<AnimateValueComponent>(&sample.declaration), nullptr,
                                 underlying ? *underlying : empty, context);
        if (!value) {
          return std::nullopt;
        }
        if (ValidPaintValue(validator, name, value->second)) {
          return PaintState(value->second);
        }
        return std::nullopt;
      });
  return !refused && sampled ? *sampled : std::nullopt;
}

template <typename P>
void ApplyPaintProperty(Registry& registry, const AnimatedValuesComponent& values, P& property,
                        const P& parent, const AnimationValueContext& context,
                        PropertyRegistry& properties) {
  if (property.specificity > css::Specificity::Animation()) {
    return;
  }
  const auto found = values.contextualLengths.find(std::string(property.name));
  if (found == values.contextualLengths.end()) {
    return;
  }
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  if (!budget.reserveContextualWork(found->second->evaluationWork)) {
    return;
  }
  const auto initial = SerializeBase(property.getInitialFn(), false, budget);
  const auto inherited = SerializeBase(parent.get(), true, budget);
  if (!initial || !inherited) {
    return;
  }
  const PaintKeywords keywords{*initial, *inherited, P::kCascadeMode != PropertyCascade::None,
                               property.name == "color"};
  const auto sampled = SamplePaintStack(
      *found->second, property.name, keywords, context, budget,
      [&] { return SerializeBase(BaseValue(property, parent), UsesParent(property), budget); });
  if (sampled) {
    std::ignore = properties.parsePresentationAttribute(property.name, *sampled, EntityHandle(),
                                                        css::Specificity::Animation());
  }
}

void SeedResourceColor(Property<css::Color>& destination, const Property<css::Color>* component) {
  if (component && component->isSpecified() &&
      (!destination.isSpecified() || component->specificity > destination.specificity)) {
    destination = *component;
  }
}

void SeedResourceColors(Registry& registry, Entity source, PropertyRegistry& properties) {
  if (const auto* stop = registry.try_get<StopComponent>(source)) {
    SeedResourceColor(properties.stopColor, &stop->properties.stopColor);
  }
  if (const auto* flood = registry.try_get<FEFloodComponent>(source)) {
    SeedResourceColor(properties.floodColor, &flood->floodColor);
  } else if (const auto* shadow = registry.try_get<FEDropShadowComponent>(source)) {
    SeedResourceColor(properties.floodColor, &shadow->floodColor);
  }
  if (const auto* diffuse = registry.try_get<FEDiffuseLightingComponent>(source)) {
    SeedResourceColor(properties.lightingColor, &diffuse->lightingColor);
  } else if (const auto* specular = registry.try_get<FESpecularLightingComponent>(source)) {
    SeedResourceColor(properties.lightingColor, &specular->lightingColor);
  }
}

}  // namespace

void ApplyPaintAnimationStyles(Registry& registry, Entity source,
                               const ComputedStyleComponent* parent, bool inheritPaint,
                               PropertyRegistry& properties) {
  const auto* enabled = registry.ctx().find<AnimationStyleContext>();
  const auto* values = registry.try_get<AnimatedValuesComponent>(source);
  if (!enabled || !enabled->enabled || !values) {
    return;
  }
  static const PropertyRegistry defaults;
  const auto& inherited = parent && parent->properties ? *parent->properties : defaults;
  const auto& paintParent = inheritPaint ? inherited : defaults;
  SeedResourceColors(registry, source, properties);
  AnimationValueContext context;
  context.linearRgb = BaseValue(properties.colorInterpolation, inherited.colorInterpolation) ==
                      ColorInterpolation::LinearRGB;
  context.currentColor = inherited.color.get()
                             .value_or(css::Color(css::RGBA::RGB(0, 0, 0)))
                             .resolve(css::RGBA::RGB(0, 0, 0), 1);
  ApplyPaintProperty(registry, *values, properties.color, inherited.color, context, properties);
  context.currentColor = BaseValue(properties.color, inherited.color)
                             .value_or(css::Color(context.currentColor))
                             .resolve(context.currentColor, 1);
  ApplyPaintProperty(registry, *values, properties.fill, paintParent.fill, context, properties);
  ApplyPaintProperty(registry, *values, properties.stroke, paintParent.stroke, context, properties);
  ApplyPaintProperty(registry, *values, properties.stopColor, inherited.stopColor, context,
                     properties);
  ApplyPaintProperty(registry, *values, properties.floodColor, inherited.floodColor, context,
                     properties);
  ApplyPaintProperty(registry, *values, properties.lightingColor, inherited.lightingColor, context,
                     properties);
}

}  // namespace donner::svg::components
