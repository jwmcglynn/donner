#include "donner/svg/components/animation/AnimationSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationMotion.h"
#include "donner/svg/components/animation/AnimationReference.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationTimeline.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/text/TextInvalidation.h"

namespace donner::svg::components {

namespace {

/// Resolve the target entity for an animation by href or parent.
Entity resolveTargetByHrefOrParent(Registry& registry, Entity animationEntity,
                                   const std::optional<std::string>& href,
                                   AnimationResourceBudget& budget) {
  if (href.has_value()) {
    std::string_view id = href.value();
    if (!id.empty() && id[0] == '#') {
      id.remove_prefix(1);
    }
    return FindAnimationReference(registry, id, budget);
  }

  if (auto* tree = registry.try_get<donner::components::TreeComponent>(animationEntity)) {
    return tree->parent();
  }

  return entt::null;
}

bool AnimatesContextAttribute(Registry& registry, Entity entity, bool reference) {
  const auto matches = [reference](std::string_view name) {
    return reference ? name == "href" || name == "xlink:href"
                     : name == "gradientUnits" || name == "maskUnits" || name == "patternUnits" ||
                           name == "filterUnits" || name == "primitiveUnits";
  };
  if (const auto* value = registry.try_get<AnimateValueComponent>(entity)) {
    return matches(value->attributeName);
  }
  if (const auto* value = registry.try_get<SetAnimationComponent>(entity)) {
    return matches(value->attributeName);
  }
  return false;
}

/// Returns true if the animation value should be applied (active or frozen).
bool shouldApplyValue(AnimationPhase phase, AnimationFill fill) {
  return phase == AnimationPhase::Active ||
         (phase == AnimationPhase::After && fill == AnimationFill::Freeze);
}

std::optional<double> CompletedIterationCount(double elapsed, double simpleDuration) {
  if (!(elapsed > 0.0) || !(simpleDuration > 0.0) || !std::isfinite(simpleDuration)) {
    return std::nullopt;
  }
  const double iterations = elapsed / simpleDuration;
  if (!std::isfinite(iterations)) {
    return std::nullopt;
  }
  const double nearestIteration = std::round(iterations);
  const double tolerance =
      8 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(iterations));
  return nearestIteration >= 1.0 && std::abs(iterations - nearestIteration) <= tolerance
             ? std::optional(nearestIteration)
             : std::nullopt;
}

/// Compute the interpolation progress [0,1] within the active interval.
/// Handles repeat iterations and keyTimes.
double computeProgress(double documentTime, double beginTime, double simpleDuration,
                       double activeDuration, bool frozen) {
  double elapsed = documentTime - beginTime;

  // Clamp to active duration.
  if (std::isfinite(activeDuration)) {
    elapsed = std::min(elapsed, activeDuration);
  }

  // Compute simple time within current iteration.
  double simpleTime = elapsed;
  if (std::isfinite(simpleDuration) && simpleDuration > 0.0) {
    // Handle repeat: simpleTime = elapsed mod simpleDuration
    simpleTime = std::fmod(elapsed, simpleDuration);
    // At the end of the last iteration, use full duration. Decimal clock values can put the
    // quotient a few ULPs from its mathematical integer, so exact remainder equality is unsafe.
    if (frozen && CompletedIterationCount(elapsed, simpleDuration)) {
      simpleTime = simpleDuration;
    }
  }

  // Compute progress [0,1] within simple duration.
  double progress = 0.0;
  if (std::isfinite(simpleDuration) && simpleDuration > 0.0) {
    progress = simpleTime / simpleDuration;
  }

  progress = std::clamp(progress, 0.0, 1.0);
  return progress;
}

struct AnimationSamplePosition {
  double progress = 0;
  double iteration = 0;
  double sampleTime = 0;
  double beginTime = 0;
  bool repeatingContentEnded = false;
  AnimationPhase phase = AnimationPhase::Before;
};

std::optional<AnimationSamplePosition> SamplePositionAtTime(const AnimationStateComponent& state,
                                                            const AnimationTimingComponent& timing,
                                                            double time) {
  const AnimationInterval* selected = nullptr;
  for (const auto& interval : state.intervals) {
    if (interval.begin > time) {
      break;
    }
    selected = &interval;
  }
  if (!selected) {
    return std::nullopt;
  }

  const AnimationPhase phase =
      time < selected->end ? AnimationPhase::Active : AnimationPhase::After;
  if (!shouldApplyValue(phase, timing.fill) ||
      (timing.fill == AnimationFill::Remove &&
       time >= selected->begin + selected->repeatDuration)) {
    return std::nullopt;
  }

  const double activeDuration = selected->end - selected->begin;
  const double sampleTime =
      phase == AnimationPhase::Active || !std::isfinite(activeDuration) ? time : selected->end;
  const double cappedTime = std::min(sampleTime, selected->begin + selected->repeatDuration);
  const double progress = computeProgress(
      cappedTime, selected->begin, state.simpleDuration, activeDuration,
      phase == AnimationPhase::After || sampleTime >= selected->begin + selected->repeatDuration);
  double iteration = 0;
  if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0) {
    const double elapsed = cappedTime - selected->begin;
    iteration = std::floor(elapsed / state.simpleDuration);
    if (progress == 1 && iteration > 0) {
      if (const auto completed = CompletedIterationCount(elapsed, state.simpleDuration)) {
        iteration = *completed - 1;
      } else {
        --iteration;
      }
    }
  }
  return AnimationSamplePosition{
      .progress = progress,
      .iteration = iteration,
      .sampleTime = cappedTime,
      .beginTime = selected->begin,
      .repeatingContentEnded = sampleTime >= selected->begin + selected->repeatDuration,
      .phase = phase};
}

bool IsPartialFrozenToOnly(const AnimationSamplePosition& sample,
                           const AnimationTimingComponent& timing,
                           const AnimateValueComponent* value) {
  return value && value->values.empty() && !value->from && value->to &&
         (sample.phase == AnimationPhase::After || sample.repeatingContentEnded) &&
         timing.fill == AnimationFill::Freeze && sample.progress < 1;
}

std::string_view AnimatedAttribute(const SetAnimationComponent* set,
                                   const AnimateValueComponent* value,
                                   const AnimateTransformComponent* transform) {
  return set ? std::string_view(set->attributeName)
             : (value ? std::string_view(value->attributeName)
                      : (transform ? std::string_view(transform->attributeName) : "transform"));
}

void AddAnimationSourceBytes(std::string_view value, std::size_t& sourceBytes) {
  if (sourceBytes <= AnimationResourceBudget::kMaximumSourceBytes) {
    sourceBytes += std::min(value.size(), AnimationResourceBudget::kMaximumSourceBytes + 1);
  }
}

void AddOptionalAnimationSourceBytes(const std::optional<std::string>& value,
                                     std::size_t& sourceBytes) {
  if (value.has_value()) {
    AddAnimationSourceBytes(*value, sourceBytes);
  }
}

std::size_t AnimationSourceBytes(const SetAnimationComponent* setComp,
                                 const AnimateValueComponent* valueComp,
                                 const AnimateTransformComponent* transformComp,
                                 const AnimateMotionComponent* motionComp) {
  std::size_t sourceBytes = 0;
  const auto addValues = [&](const auto& component) {
    sourceBytes += (component.keyTimes.size() + component.keySplines.size()) * sizeof(double);
    AddOptionalAnimationSourceBytes(component.from, sourceBytes);
    AddOptionalAnimationSourceBytes(component.to, sourceBytes);
    AddOptionalAnimationSourceBytes(component.by, sourceBytes);
    for (const std::string& value : component.values) {
      AddAnimationSourceBytes(value, sourceBytes);
      if (sourceBytes > AnimationResourceBudget::kMaximumSourceBytes) {
        break;
      }
    }
  };
  if (setComp) {
    AddAnimationSourceBytes(setComp->attributeName, sourceBytes);
    AddOptionalAnimationSourceBytes(setComp->to, sourceBytes);
    AddOptionalAnimationSourceBytes(setComp->href, sourceBytes);
  } else if (valueComp) {
    addValues(*valueComp);
    AddAnimationSourceBytes(valueComp->attributeName, sourceBytes);
    AddOptionalAnimationSourceBytes(valueComp->href, sourceBytes);
  } else if (transformComp) {
    addValues(*transformComp);
    AddAnimationSourceBytes(transformComp->attributeName, sourceBytes);
    AddOptionalAnimationSourceBytes(transformComp->href, sourceBytes);
  } else if (motionComp) {
    addValues(*motionComp);
    AddOptionalAnimationSourceBytes(motionComp->path, sourceBytes);
    AddAnimationSourceBytes(motionComp->rotate, sourceBytes);
    AddOptionalAnimationSourceBytes(motionComp->href, sourceBytes);
    sourceBytes += motionComp->keyPoints.size() * sizeof(double);
  }
  return sourceBytes;
}

/// The `href` of an animation element, taken from whichever animation component carries it.
const std::optional<std::string>& AnimationHref(Registry& registry, Entity entity) {
  if (const auto* setComp = registry.try_get<SetAnimationComponent>(entity)) {
    return setComp->href;
  }
  if (const auto* valueComp = registry.try_get<AnimateValueComponent>(entity)) {
    return valueComp->href;
  }
  if (const auto* motion = registry.try_get<AnimateMotionComponent>(entity)) {
    return motion->href;
  }
  return registry.get<AnimateTransformComponent>(entity).href;
}

/// Resolve live authored targeting without refreshing typed animation state under a read lock.
Entity ResolveAuthoredAnimationTarget(Registry& registry, Entity entity,
                                      AnimationResourceBudget& budget) {
  const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
  if (!attributes) {
    return resolveTargetByHrefOrParent(registry, entity, AnimationHref(registry, entity), budget);
  }
  auto href = attributes->getAttribute("href");
  if (!href) {
    href = attributes->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
  }
  if (!href) {
    return resolveTargetByHrefOrParent(registry, entity, std::nullopt, budget);
  }
  if (!budget.reserveSourceBytes(href->size())) {
    return entt::null;
  }
  return resolveTargetByHrefOrParent(registry, entity, std::string(std::string_view(*href)),
                                     budget);
}

bool ValidTransformAttribute(Registry& registry, Entity target, std::string_view name) {
  const auto* type = registry.try_get<ElementTypeComponent>(target);
  if (!type) {
    return false;
  }
  if (name == "transform") {
    return true;
  }
  if (name == "gradientTransform") {
    return type->type() == ElementType::LinearGradient ||
           type->type() == ElementType::RadialGradient;
  }
  return name == "patternTransform" && type->type() == ElementType::Pattern;
}

using ContextualRecipes =
    std::unordered_map<Entity,
                       std::unordered_map<std::string, std::vector<ContextualLengthSample>>>;

constexpr std::size_t kMaximumFrozenSandwichDepth = 256;

struct FrozenLowerLayer {
  Entity entity = entt::null;
  AnimationSamplePosition sample;
  const SetAnimationComponent* set = nullptr;
  const AnimateValueComponent* value = nullptr;
  const AnimateTransformComponent* transform = nullptr;
};

bool NeedsInstanceValueContext(Registry& registry, Entity target, std::string_view attribute) {
  if (attribute == "font-size" || IsContextualScalarAttribute(attribute) ||
      IsContextualPaintAttribute(attribute)) {
    return true;
  }
  const auto* type = registry.try_get<ElementTypeComponent>(target);
  if (!type) {
    return false;
  }
  if (registry.all_of<FilterPrimitiveComponent>(target) &&
      (attribute == "x" || attribute == "y" || attribute == "width" || attribute == "height")) {
    return true;
  }
  const bool shapeLength =
      (type->type() == ElementType::Rect &&
       (attribute == "x" || attribute == "y" || attribute == "width" || attribute == "height" ||
        attribute == "rx" || attribute == "ry")) ||
      (type->type() == ElementType::Circle &&
       (attribute == "cx" || attribute == "cy" || attribute == "r")) ||
      (type->type() == ElementType::Ellipse &&
       (attribute == "cx" || attribute == "cy" || attribute == "rx" || attribute == "ry")) ||
      (type->type() == ElementType::Line &&
       (attribute == "x1" || attribute == "y1" || attribute == "x2" || attribute == "y2"));
  const bool textLength =
      attribute == "textLength" &&
      (type->type() == ElementType::Text || type->type() == ElementType::TSpan ||
       type->type() == ElementType::TextPath || type->type() == ElementType::A);
  const bool textPosition = (attribute == "x" || attribute == "y" || attribute == "dx" ||
                             attribute == "dy" || attribute == "rotate") &&
                            (type->type() == ElementType::Text ||
                             type->type() == ElementType::TSpan || type->type() == ElementType::A);
  return shapeLength || textLength || textPosition ||
         (type->type() == ElementType::TextPath && attribute == "startOffset") ||
         (type->type() == ElementType::Marker &&
          (attribute == "markerWidth" || attribute == "markerHeight" || attribute == "refX" ||
           attribute == "refY"));
}

bool AppendContextualLengthSample(std::vector<ContextualLengthSample>& samples,
                                  const SetAnimationComponent* set,
                                  const AnimateValueComponent* value, double progress,
                                  double iteration, bool usesFrozenUnderlying,
                                  std::shared_ptr<const ContextualLengthRecipe> frozenUnderlying,
                                  AnimationResourceBudget& budget) {
  const std::size_t sourceBytes = AnimationSourceBytes(set, value, nullptr, nullptr);
  const std::size_t keyframes = value ? value->values.size() : 0;
  if (keyframes > AnimationResourceBudget::kMaximumOutputBytes / (2 * sizeof(std::string))) {
    return budget.reserveOutput(AnimationResourceBudget::kMaximumOutputBytes + 1);
  }
  const std::size_t retained =
      2 * (sizeof(ContextualLengthSample) + sourceBytes + keyframes * sizeof(std::string)) + 256;
  if (!budget.reserveOutput(retained)) {
    return false;
  }
  ContextualLengthSample sample;
  sample.progress = progress;
  sample.iteration = iteration;
  sample.usesFrozenUnderlying = usesFrozenUnderlying;
  sample.frozenUnderlying = std::move(frozenUnderlying);
  if (set) {
    sample.declaration = *set;
  } else {
    sample.declaration = *value;
  }
  samples.push_back(std::move(sample));
  return true;
}

bool RetainContextualLength(ContextualRecipes& recipes, Entity target, std::string_view attribute,
                            const SetAnimationComponent* set, const AnimateValueComponent* value,
                            double progress, double iteration, bool usesFrozenUnderlying,
                            std::shared_ptr<const ContextualLengthRecipe> frozenUnderlying,
                            AnimationResourceBudget& budget) {
  auto& samples = recipes[target][std::string(attribute)];
  return AppendContextualLengthSample(samples, set, value, progress, iteration,
                                      usesFrozenUnderlying, std::move(frozenUnderlying), budget);
}

std::optional<std::vector<FrozenLowerLayer>> CollectFrozenLowerLayers(
    Registry& registry, std::span<const Entity> documentAnimations, Entity upperAnimation,
    const AnimationSamplePosition& upperSample, Entity target, std::string_view attribute,
    bool contextualOnly, AnimationResourceBudget& budget) {
  const auto upperOrder =
      std::find(documentAnimations.begin(), documentAnimations.end(), upperAnimation);
  if (upperOrder == documentAnimations.end()) {
    return std::vector<FrozenLowerLayer>{};
  }

  std::vector<FrozenLowerLayer> result;
  for (auto it = documentAnimations.begin(); it != documentAnimations.end(); ++it) {
    const Entity entity = *it;
    if (entity == upperAnimation || !budget.reserveReferenceWork(attribute.size() + 1)) {
      if (budget.rejected()) {
        return std::nullopt;
      }
      continue;
    }
    const auto* set = registry.try_get<SetAnimationComponent>(entity);
    const auto* value = registry.try_get<AnimateValueComponent>(entity);
    const auto* transform = registry.try_get<AnimateTransformComponent>(entity);
    if ((!set && !value && !transform) || (contextualOnly && !set && !value) ||
        AnimatedAttribute(set, value, transform) != attribute) {
      continue;
    }
    auto& state = registry.get<AnimationStateComponent>(entity);
    state.targetEntity = ResolveAuthoredAnimationTarget(registry, entity, budget);
    if (budget.rejected()) {
      return std::nullopt;
    }
    if (state.targetEntity != target) {
      continue;
    }
    const auto sample = SamplePositionAtTime(state, registry.get<AnimationTimingComponent>(entity),
                                             upperSample.sampleTime);
    if (!sample || sample->beginTime > upperSample.beginTime ||
        (sample->beginTime == upperSample.beginTime && it >= upperOrder)) {
      continue;
    }
    result.push_back(FrozenLowerLayer{
        .entity = entity, .sample = *sample, .set = set, .value = value, .transform = transform});
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
    return first.sample.beginTime < second.sample.beginTime;
  });
  return result;
}

std::shared_ptr<const ContextualLengthRecipe> BuildFrozenContextualUnderlying(
    Registry& registry, std::span<const Entity> documentAnimations, Entity upperAnimation,
    const AnimationSamplePosition& upperSample, Entity target, std::string_view attribute,
    AnimationResourceBudget& budget, std::size_t depth) {
  if (depth >= kMaximumFrozenSandwichDepth) {
    budget.reserveOutput(AnimationResourceBudget::kMaximumOutputBytes + 1);
    return nullptr;
  }

  const auto lowerLayers = CollectFrozenLowerLayers(registry, documentAnimations, upperAnimation,
                                                    upperSample, target, attribute, true, budget);
  if (!lowerLayers) {
    return nullptr;
  }
  std::vector<ContextualLengthSample> samples;
  for (const auto& layer : *lowerLayers) {
    const auto& timing = registry.get<AnimationTimingComponent>(layer.entity);
    std::shared_ptr<const ContextualLengthRecipe> frozenUnderlying;
    const bool usesFrozenUnderlying = IsPartialFrozenToOnly(layer.sample, timing, layer.value);
    if (usesFrozenUnderlying) {
      frozenUnderlying =
          BuildFrozenContextualUnderlying(registry, documentAnimations, layer.entity, layer.sample,
                                          target, attribute, budget, depth + 1);
      if (budget.rejected()) {
        return nullptr;
      }
    }
    if (!AppendContextualLengthSample(samples, layer.set, layer.value, layer.sample.progress,
                                      layer.sample.iteration, usesFrozenUnderlying,
                                      std::move(frozenUnderlying), budget)) {
      return nullptr;
    }
  }
  if (samples.empty()) {
    return nullptr;
  }
  auto result = ContextualLengthRecipe::Create(std::move(samples));
  if (!result) {
    budget.reserveOutput(AnimationResourceBudget::kMaximumOutputBytes + 1);
  }
  return result;
}

std::optional<std::string> RebuildFrozenUnderlyingValue(
    Registry& registry, std::span<const Entity> documentAnimations, Entity upperAnimation,
    const AnimationSamplePosition& upperSample, Entity target, std::string_view attribute,
    const std::string& base, const AnimationValueContext& context, AnimationResourceBudget& budget,
    std::size_t depth) {
  if (depth >= kMaximumFrozenSandwichDepth) {
    budget.reserveOutput(AnimationResourceBudget::kMaximumOutputBytes + 1);
    return std::nullopt;
  }

  const auto lowerLayers = CollectFrozenLowerLayers(registry, documentAnimations, upperAnimation,
                                                    upperSample, target, attribute, false, budget);
  if (!lowerLayers) {
    return std::nullopt;
  }
  std::string underlying = base;
  for (const auto& layer : *lowerLayers) {
    const auto& timing = registry.get<AnimationTimingComponent>(layer.entity);
    std::string layerUnderlying = underlying;
    if (IsPartialFrozenToOnly(layer.sample, timing, layer.value)) {
      auto frozen =
          RebuildFrozenUnderlyingValue(registry, documentAnimations, layer.entity, layer.sample,
                                       target, attribute, base, context, budget, depth + 1);
      if (!frozen) {
        return std::nullopt;
      }
      layerUnderlying = std::move(*frozen);
    }
    if (!budget.reserveSourceBytes(
            AnimationSourceBytes(layer.set, layer.value, layer.transform, nullptr))) {
      return std::nullopt;
    }
    const auto sampled =
        SampleAnimationValue(layer.sample.progress, layer.sample.iteration, layer.set, layer.value,
                             layer.transform, layerUnderlying, context, &budget);
    if (budget.rejected()) {
      return std::nullopt;
    }
    if (!sampled) {
      continue;
    }
    const bool acceptsEmpty = sampled->first == "href" || sampled->first == "xlink:href" ||
                              sampled->first == "result" || sampled->first == "d";
    if (sampled->second.empty() && !acceptsEmpty) {
      continue;
    }
    if (!budget.reserveOutput(sampled->second.size())) {
      return std::nullopt;
    }
    underlying = sampled->second;
  }
  return underlying;
}

bool ProcessContextualAnimation(Registry& registry, std::span<const Entity> documentAnimations,
                                Entity entity, const AnimationSamplePosition& sample,
                                const AnimationTimingComponent& timing, Entity target,
                                std::string_view attribute, const SetAnimationComponent* setComp,
                                const AnimateValueComponent* valueComp, ContextualRecipes& recipes,
                                AnimationResourceBudget& budget) {
  std::shared_ptr<const ContextualLengthRecipe> frozenUnderlying;
  const bool partial = IsPartialFrozenToOnly(sample, timing, valueComp);
  if (partial) {
    frozenUnderlying = BuildFrozenContextualUnderlying(registry, documentAnimations, entity, sample,
                                                       target, attribute, budget, 0);
    if (budget.rejected()) {
      return false;
    }
  }
  return RetainContextualLength(recipes, target, attribute, setComp, valueComp, sample.progress,
                                sample.iteration, partial, std::move(frozenUnderlying), budget);
}

bool NeedsUnderlyingValue(const AnimateValueComponent* value,
                          const AnimateTransformComponent* transform,
                          const AnimateMotionComponent* motion) {
  return motion || (value && (value->additive || (value->values.empty() && !value->from))) ||
         (transform && (transform->additive || (!transform->from && transform->by)));
}

bool UsesContextualValue(Registry& registry, Entity target, std::string_view attribute,
                         const SetAnimationComponent* set, const AnimateValueComponent* value) {
  return (set || value) && NeedsInstanceValueContext(registry, target, attribute);
}

std::optional<std::string> ResolveOrdinaryUnderlying(
    Registry& registry, std::span<const Entity> documentAnimations, Entity entity,
    const AnimationSamplePosition& sample, const AnimationTimingComponent& timing, Entity target,
    std::string_view attribute, const AnimateValueComponent* valueComp,
    const AnimateTransformComponent* transformComp, const AnimateMotionComponent* motionComp,
    const AnimationValueContext& context, AnimationResourceBudget& budget) {
  const bool needsUnderlying = NeedsUnderlyingValue(valueComp, transformComp, motionComp);
  auto underlying = needsUnderlying
                        ? UnderlyingAnimationValue(registry, target, attribute, context, budget)
                        : std::optional(std::string{});
  if (budget.rejected()) {
    return std::nullopt;
  }
  if (!underlying || !IsPartialFrozenToOnly(sample, timing, valueComp)) {
    return underlying;
  }
  const auto base = BaseAnimationValue(registry, target, attribute, context, budget);
  if (budget.rejected() || !base) {
    return std::nullopt;
  }
  return RebuildFrozenUnderlyingValue(registry, documentAnimations, entity, sample, target,
                                      attribute, *base, context, budget, 0);
}

std::optional<std::pair<std::string, std::string>> SampleOrdinaryOverride(
    Registry& registry, Entity entity, Entity target, const AnimationSamplePosition& sample,
    const SetAnimationComponent* setComp, const AnimateValueComponent* valueComp,
    const AnimateTransformComponent* transformComp, const AnimateMotionComponent* motionComp,
    const std::string& underlying, const AnimationValueContext& context,
    std::unordered_map<Entity, std::string>& motionBases, AnimationResourceBudget& budget) {
  if (!motionComp) {
    return SampleAnimationValue(sample.progress, sample.iteration, setComp, valueComp,
                                transformComp, underlying, context, &budget);
  }
  auto motionSample = SampleAnimationMotion(registry, entity, *motionComp, sample.progress,
                                            sample.iteration, budget);
  std::string& motion = motionSample.transform;
  const auto [base, inserted] = motionBases.try_emplace(target, underlying);
  const std::string& prefix = motionSample.additive ? underlying : base->second;
  if (!motion.empty() && !prefix.empty()) {
    motion = prefix + " " + motion;
  }
  return std::pair<std::string, std::string>{"transform", std::move(motion)};
}

bool StoreAnimationOverride(Registry& registry, Entity target,
                            std::optional<std::pair<std::string, std::string>> sampled,
                            AnimationResourceBudget& budget) {
  if (budget.rejected()) {
    return false;
  }
  if (!sampled) {
    return true;
  }
  const bool acceptsEmpty = sampled->first == "href" || sampled->first == "xlink:href" ||
                            sampled->first == "result" || sampled->first == "d";
  if (sampled->second.empty() && !acceptsEmpty) {
    return true;
  }
  if (!budget.reserveOutput(sampled->second.size())) {
    return false;
  }
  registry.get_or_emplace<AnimatedValuesComponent>(target).overrides[std::string(sampled->first)] =
      std::move(sampled->second);
  return true;
}

bool ProcessAnimation(Registry& registry, Entity entity, double documentTime,
                      AnimationResourceBudget& resourceBudget,
                      std::unordered_map<Entity, std::string>& motionBases,
                      ContextualRecipes& recipes, std::span<const Entity> documentAnimations) {
  const auto& timing = registry.get<AnimationTimingComponent>(entity);
  auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
  const auto* setComp = registry.try_get<SetAnimationComponent>(entity);
  const auto* valueComp = registry.try_get<AnimateValueComponent>(entity);
  const auto* transformComp = registry.try_get<AnimateTransformComponent>(entity);
  const auto* motionComp = registry.try_get<AnimateMotionComponent>(entity);

  if (!resourceBudget.reserveAnimation(
          AnimationSourceBytes(setComp, valueComp, transformComp, motionComp))) {
    return false;
  }
  state.targetEntity = resolveTargetByHrefOrParent(registry, entity,
                                                   AnimationHref(registry, entity), resourceBudget);
  if (resourceBudget.rejected()) {
    return false;
  }
  if (!registry.valid(state.targetEntity)) {
    return true;
  }
  if (!IsAnimationInSvgNamespace(registry, state.targetEntity)) {
    return true;
  }

  if (transformComp &&
      !ValidTransformAttribute(registry, state.targetEntity, transformComp->attributeName)) {
    return true;
  }

  const auto sample = SamplePositionAtTime(state, timing, documentTime);
  if (!sample) {
    return true;
  }
  const std::string_view attribute = AnimatedAttribute(setComp, valueComp, transformComp);
  if (UsesContextualValue(registry, state.targetEntity, attribute, setComp, valueComp)) {
    return ProcessContextualAnimation(registry, documentAnimations, entity, *sample, timing,
                                      state.targetEntity, attribute, setComp, valueComp, recipes,
                                      resourceBudget);
  }
  const auto context =
      setComp ? AnimationValueContext{}
              : AnimationContextFor(registry, state.targetEntity, attribute, resourceBudget);
  auto underlyingValue = ResolveOrdinaryUnderlying(
      registry, documentAnimations, entity, *sample, timing, state.targetEntity, attribute,
      valueComp, transformComp, motionComp, context, resourceBudget);
  if (!underlyingValue) {
    return !resourceBudget.rejected();
  }
  auto sampled = SampleOrdinaryOverride(registry, entity, state.targetEntity, *sample, setComp,
                                        valueComp, transformComp, motionComp, *underlyingValue,
                                        context, motionBases, resourceBudget);
  return StoreAnimationOverride(registry, state.targetEntity, std::move(sampled), resourceBudget);
}

void ClearSampledAnimationValues(Registry& registry,
                                 std::unordered_set<Entity>* invalidatedText = nullptr) {
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    const bool hadContextualValues = !values.contextualLengths.empty();
    values.overrides.clear();
    values.contextualLengths.clear();
    if (hadContextualValues && (!invalidatedText || invalidatedText->insert(entity).second)) {
      InvalidateAnimatedText(registry, entity);
    }
  }
}

void PublishContextualRecipes(Registry& registry, ContextualRecipes& recipes,
                              const AnimationResourceBudget& budget,
                              std::unordered_set<Entity>& invalidatedText) {
  if (budget.rejected()) {
    return;
  }
  for (auto& [target, attributes] : recipes) {
    auto& values = registry.get_or_emplace<AnimatedValuesComponent>(target);
    for (auto& [attribute, samples] : attributes) {
      const auto recipe = ContextualLengthRecipe::Create(std::move(samples));
      if (!recipe) {
        ClearSampledAnimationValues(registry, &invalidatedText);
        return;
      }
      values.contextualLengths.emplace(attribute, recipe);
      if (invalidatedText.insert(target).second) {
        InvalidateAnimatedText(registry, target);
      }
    }
  }
}

void RemoveEmptyAnimationValues(Registry& registry) {
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    if (values.overrides.empty() && values.contextualLengths.empty()) {
      registry.remove<AnimatedValuesComponent>(entity);
    }
  }
}

std::string_view MouseEventName(EventType type) {
  switch (type) {
    case EventType::Click: return "click";
    case EventType::DblClick: return "dblclick";
    case EventType::MouseDown: return "mousedown";
    case EventType::MouseUp: return "mouseup";
    case EventType::MouseMove: return "mousemove";
    case EventType::MouseEnter: return "mouseenter";
    case EventType::MouseLeave: return "mouseleave";
    case EventType::MouseOver: return "mouseover";
    case EventType::MouseOut: return "mouseout";
    default: return {};
  }
}

std::unordered_set<Entity> MouseEventPath(Registry& registry, Entity target, bool bubbles) {
  std::unordered_set<Entity> path;
  constexpr std::size_t kMaximumEventDepth = 4096;
  while (registry.valid(target) && path.size() < kMaximumEventDepth && path.insert(target).second) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(target);
    if (!bubbles || tree == nullptr) {
      break;
    }
    target = tree->parent();
  }
  return path;
}

struct PendingAnimationEvent {
  Entity animation;
  bool begin;
  std::vector<double> instances;
};

bool ReservePendingHistory(std::vector<PendingAnimationEvent>& pending,
                           AnimationResourceBudget& budget) {
  if (pending.size() == pending.capacity()) {
    const std::size_t capacity = std::max<std::size_t>(16, pending.capacity() * 2);
    if (!budget.reserveOutput(capacity * sizeof(PendingAnimationEvent))) {
      return false;
    }
    pending.reserve(capacity);
  }
  return budget.reserveOutput(1024 * sizeof(double));
}

bool StageEventInstance(Registry& registry, Entity animation, bool begin, double time,
                        AnimationResourceBudget& budget,
                        std::vector<PendingAnimationEvent>& pending,
                        std::optional<std::size_t>& historyIndex) {
  if (!std::isfinite(time)) {
    return true;
  }
  const auto& state = registry.get<AnimationStateComponent>(animation);
  const auto& original = begin ? state.eventBeginTimes : state.eventEndTimes;
  const auto& instances = historyIndex ? pending[*historyIndex].instances : original;
  if (!budget.reserveReferenceWork(2 * instances.size() + 1)) {
    return false;
  }
  if (std::binary_search(instances.begin(), instances.end(), time)) {
    return true;
  }
  if (instances.size() >= 1024) {
    return false;
  }
  if (!historyIndex) {
    if (!ReservePendingHistory(pending, budget)) {
      return false;
    }
    PendingAnimationEvent history{animation, begin, {}};
    history.instances.reserve(1024);
    history.instances.assign(original.begin(), original.end());
    historyIndex = pending.size();
    pending.push_back(std::move(history));
  }
  return InsertAnimationInstance(pending[*historyIndex].instances, time);
}

bool StageMatchingEvents(Registry& registry, Entity animation,
                         const std::optional<std::string>& value, Entity defaultTarget,
                         std::string_view event, const std::unordered_set<Entity>& path,
                         double time, bool begin, bool futureOnly, AnimationResourceBudget& budget,
                         std::vector<PendingAnimationEvent>& pending) {
  if (!value) {
    return true;
  }
  const auto specs = ParseAnimationTimeList(*value);
  if (!specs) {
    return false;
  }
  std::optional<std::size_t> historyIndex;
  for (const auto& spec : *specs) {
    if (spec.kind != AnimationTimeSpecifier::Kind::Event || spec.event != event) {
      continue;
    }
    const Entity source = spec.sourceId.empty()
                              ? defaultTarget
                              : FindAnimationReference(registry, spec.sourceId, budget);
    if (budget.rejected()) {
      return false;
    }
    if (path.contains(source) && (!futureOnly || spec.offset > 0) &&
        !StageEventInstance(registry, animation, begin, time + spec.offset, budget, pending,
                            historyIndex)) {
      return false;
    }
  }
  return true;
}

bool StageAnimationMouseEvent(Registry& registry, Entity entity,
                              const AnimationTimingComponent& timing, std::string_view eventName,
                              const std::unordered_set<Entity>& path, double time,
                              AnimationResourceBudget& budget,
                              std::vector<PendingAnimationEvent>& pending) {
  const auto& state = registry.get<AnimationStateComponent>(entity);
  const Entity target =
      resolveTargetByHrefOrParent(registry, entity, AnimationHref(registry, entity), budget);
  return !budget.rejected() &&
         StageMatchingEvents(registry, entity, timing.endValue, target, eventName, path, time,
                             false, state.phase != AnimationPhase::Active, budget, pending) &&
         StageMatchingEvents(registry, entity, timing.beginValue, target, eventName, path, time,
                             true, false, budget, pending);
}

bool CommitAnimationEvents(Registry& registry, std::vector<PendingAnimationEvent>& pending) {
  for (auto& event : pending) {
    auto& state = registry.get<AnimationStateComponent>(event.animation);
    auto& instances = event.begin ? state.eventBeginTimes : state.eventEndTimes;
    instances.swap(event.instances);
  }
  return !pending.empty();
}

}  // namespace

bool IsAnimationInSvgNamespace(Registry& registry, Entity entity) {
  const auto& namespaces = registry.ctx().get<xml::components::XMLNamespaceContext>();
  for (std::size_t depth = 0; depth < 4096; ++depth) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    if (!tree) {
      return false;
    }
    const auto uri = namespaces.getNamespaceUri(registry, entity, tree->tagName().namespacePrefix);
    if ((uri && *uri != "http://www.w3.org/2000/svg") ||
        (!uri && !tree->tagName().namespacePrefix.empty())) {
      return false;
    }
    if (tree->parent() == entt::null) {
      return true;
    }
    entity = tree->parent();
  }
  return false;
}

bool CollectAnimationEntity(Registry& registry, Entity current, AnimationCollectionMode mode,
                            std::size_t& declarations, std::vector<Entity>& result) {
  if (!registry.any_of<SetAnimationComponent, AnimateValueComponent, AnimateTransformComponent,
                       AnimateMotionComponent>(current)) {
    return true;
  }
  if (++declarations > AnimationResourceBudget::kMaximumAnimations) {
    result.resize(AnimationResourceBudget::kMaximumAnimations + 1, entt::null);
    return false;
  }
  if (HasSvgNamespace(registry, current) && IsAnimationInSvgNamespace(registry, current)) {
    result.push_back(current);
  } else if (mode == AnimationCollectionMode::DiscardExcludedState) {
    registry.remove<AnimationStateComponent>(current);
  }
  return result.size() <= AnimationResourceBudget::kMaximumAnimations;
}

Entity NextAnimationTraversalEntity(Registry& registry, Entity current, Entity root) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
  if (!tree) {
    return entt::null;
  }
  if (tree->firstChild() != entt::null) {
    return tree->firstChild();
  }
  while (tree && tree->nextSibling() == entt::null) {
    if (current == root) {
      return entt::null;
    }
    current = tree->parent();
    tree = registry.try_get<donner::components::TreeComponent>(current);
  }
  return tree ? tree->nextSibling() : entt::null;
}

std::vector<Entity> CollectAnimationEntities(Registry& registry, AnimationCollectionMode mode) {
  std::vector<Entity> result;
  const Entity root = registry.ctx().get<SVGDocumentContext>().rootEntity;
  Entity current = root;
  std::size_t visited = 0;
  std::size_t declarations = 0;
  while (current != entt::null && ++visited <= 1024 * 1024) {
    if (!CollectAnimationEntity(registry, current, mode, declarations, result)) {
      break;
    }
    current = NextAnimationTraversalEntity(registry, current, root);
  }
  if (current != entt::null && visited > 1024 * 1024) {
    result.resize(AnimationResourceBudget::kMaximumAnimations + 1, entt::null);
  }
  return result;
}

bool AnimationSystem::isAnimationTarget(Registry& registry, Entity targetEntity) const {
  if (!IsEntityAttachedToDocument(registry, targetEntity) ||
      !IsAnimationInSvgNamespace(registry, targetEntity)) {
    return false;
  }

  const auto animations = CollectAnimationEntities(registry);
  if (animations.size() > AnimationResourceBudget::kMaximumAnimations) {
    return true;
  }
  AnimationResourceBudget budget;
  for (const Entity entity : animations) {
    const Entity resolved = ResolveAuthoredAnimationTarget(registry, entity, budget);
    if (budget.rejected()) {
      return true;
    }
    if (resolved == targetEntity) {
      return true;
    }
  }

  return false;
}

bool AnimationSystem::dispatchMouseEvent(Registry& registry, Entity targetEntity, EventType type,
                                         double documentTime) {
  return dispatchEvent(registry, targetEntity, MouseEventName(type), eventBubbles(type),
                       documentTime);
}

bool AnimationSystem::dispatchEvent(Registry& registry, Entity targetEntity, std::string_view name,
                                    bool bubbles, double documentTime) {
  if (name.empty() || name.size() > 128 || !registry.valid(targetEntity) ||
      !std::isfinite(documentTime)) {
    return false;
  }
  const auto animations =
      CollectAnimationEntities(registry, AnimationCollectionMode::DiscardExcludedState);
  AnimationResourceBudget budget;
  if (!ResolveAnimationTimeline(registry, animations, documentTime, budget)) {
    return false;
  }
  const auto path = MouseEventPath(registry, targetEntity, bubbles);
  std::vector<PendingAnimationEvent> pending;
  for (Entity entity : animations) {
    const auto& timing = registry.get<AnimationTimingComponent>(entity);
    std::size_t sourceBytes = 0;
    AddOptionalAnimationSourceBytes(timing.beginValue, sourceBytes);
    AddOptionalAnimationSourceBytes(timing.endValue, sourceBytes);
    if (!budget.reserveAnimation(sourceBytes) ||
        !StageAnimationMouseEvent(registry, entity, timing, name, path, documentTime, budget,
                                  pending)) {
      return false;
    }
  }
  return CommitAnimationEvents(registry, pending);
}

void AnimationSystem::advance(Registry& registry, double documentTime,
                              std::vector<ParseDiagnostic>* /*outWarnings*/) {
  if (registry.ctx().contains<AnimationResourceBudget>()) {
    registry.ctx().erase<AnimationResourceBudget>();
  }
  auto& resourceBudget = registry.ctx().emplace<AnimationResourceBudget>();
  if (const auto* context = registry.ctx().find<AnimationContextualWorkContext>()) {
    resourceBudget.setContextualWorkBudget(context->budget);
  }
  std::unordered_set<Entity> invalidatedText;
  ClearSampledAnimationValues(registry, &invalidatedText);

  if (registry.ctx().get<ResourceManagerContext>().processingMode() ==
      ProcessingMode::SecureStatic) {
    registry.clear<AnimatedValuesComponent>();
    return;
  }
  auto animations =
      CollectAnimationEntities(registry, AnimationCollectionMode::DiscardExcludedState);
  if (!ResolveAnimationTimeline(registry, animations, documentTime, resourceBudget)) {
    return;
  }
  const std::vector<Entity> documentAnimations = animations;
  std::stable_sort(animations.begin(), animations.end(), [&](Entity first, Entity second) {
    return registry.get<AnimationStateComponent>(first).beginTime <
           registry.get<AnimationStateComponent>(second).beginTime;
  });
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return !registry.all_of<AnimateMotionComponent>(entity);
  });
  // Coordinate interpolation needs the final sampled unit selection, including inherited units.
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return AnimatesContextAttribute(registry, entity, false);
  });
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return AnimatesContextAttribute(registry, entity, true);
  });
  std::unordered_map<Entity, std::string> motionBases;
  ContextualRecipes recipes;
  for (std::size_t index = 0; index < animations.size(); ++index) {
    if (!ProcessAnimation(registry, animations[index], documentTime, resourceBudget, motionBases,
                          recipes, documentAnimations)) {
      ClearSampledAnimationValues(registry, &invalidatedText);
      recipes.clear();
      break;
    }
  }

  PublishContextualRecipes(registry, recipes, resourceBudget, invalidatedText);
  RemoveEmptyAnimationValues(registry);
}

}  // namespace donner::svg::components
