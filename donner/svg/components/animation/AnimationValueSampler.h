#pragma once
/// @file

#include <optional>
#include <string>
#include <utility>

#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationValueMath.h"
#include "donner/svg/components/animation/ContextualLengthRecipe.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"

namespace donner::svg::components {

class AnimationResourceBudget;

namespace detail {

/// Recursive implementation of \ref donner::svg::components::SampleContextualAnimationRecipe.
/// @param recipe Admitted stack to fold.
/// @param base Value that frozen-underlying layers start from.
/// @param underlying Value beneath the first layer.
/// @param sampler Callable(sample, lowerValue) returning a contributed value or nullopt.
template <typename Value, typename Sampler>
std::optional<Value> SampleContextualAnimationRecipeImpl(const ContextualLengthRecipe& recipe,
                                                         const Value& base, Value underlying,
                                                         Sampler& sampler) {
  std::optional<Value> result;
  for (const auto& sample : recipe.samples) {
    Value layerUnderlying = underlying;
    if (sample.usesFrozenUnderlying) {
      layerUnderlying = base;
    }
    if (sample.frozenUnderlying) {
      if (auto frozen =
              SampleContextualAnimationRecipeImpl(*sample.frozenUnderlying, base, base, sampler)) {
        layerUnderlying = std::move(*frozen);
      }
    }
    if (auto value = sampler(sample, layerUnderlying)) {
      underlying = *value;
      result = std::move(*value);
    }
  }
  return result;
}

}  // namespace detail

/** Fold an admitted contextual animation stack, rebuilding frozen to-only underliers as needed.
 * @tparam Value Owning consumer value copied at frozen sandwich boundaries.
 * @param recipe Immutable admitted stack.
 * @param underlying Authored, styled, or typed base for the referencing instance.
 * @param sampler Callable(sample, lowerValue) returning a valid contributed value or nullopt.
 * @return Last valid contribution, or nullopt when no layer contributes.
 */
template <typename Value, typename Sampler>
std::optional<Value> SampleContextualAnimationRecipe(const ContextualLengthRecipe& recipe,
                                                     Value underlying, Sampler&& sampler) {
  return detail::SampleContextualAnimationRecipeImpl(recipe, underlying, underlying, sampler);
}

/** Sample one animation without document or timing side effects.
 * @param progress Progress within the current iteration.
 * @param iteration Number of completed iterations.
 * @param setComp Set declaration, or null.
 * @param valueComp Value declaration, or null.
 * @param transformComp Transform declaration, or null. Exactly one declaration must be supplied.
 * @param underlying Current lower-layer value.
 * @param context Coordinate and color context.
 * @param budget Optional shared path admission; borrowed only during this call.
 * @return Owning attribute/value pair, or nullopt when the declaration cannot be sampled.
 */
std::optional<std::pair<std::string, std::string>> SampleAnimationValue(
    double progress, double iteration, const SetAnimationComponent* setComp,
    const AnimateValueComponent* valueComp, const AnimateTransformComponent* transformComp,
    const std::string& underlying, const AnimationValueContext& context,
    AnimationResourceBudget* budget = nullptr);

/** Evaluate an admitted contextual length stack with the shared interpolation rules.
 * @param recipe Immutable, previously admitted samples in sandwich order.
 * @param underlying Authored or typed base value in this instance.
 * @param context Actual percentage and font context.
 * @return Final finite single length, or nullopt when no layer contributes.
 * The caller must admit recipe.evaluationWork before invoking this operation.
 */
std::optional<Lengthd> SampleContextualLengthRecipe(const ContextualLengthRecipe& recipe,
                                                    Lengthd underlying,
                                                    const AnimationValueContext& context);

}  // namespace donner::svg::components
