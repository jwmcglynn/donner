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

/** Sample one animation without document or timing side effects.
 * @param progress Progress within the current iteration.
 * @param iteration Number of completed iterations.
 * @param setComp Set declaration, or null.
 * @param valueComp Value declaration, or null.
 * @param transformComp Transform declaration, or null. Exactly one declaration must be supplied.
 * @param underlying Current lower-layer value.
 * @param context Coordinate and color context.
 * @return Owning attribute/value pair, or nullopt when the declaration cannot be sampled.
 */
std::optional<std::pair<std::string, std::string>> SampleAnimationValue(
    double progress, double iteration, const SetAnimationComponent* setComp,
    const AnimateValueComponent* valueComp, const AnimateTransformComponent* transformComp,
    const std::string& underlying, const AnimationValueContext& context);

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
