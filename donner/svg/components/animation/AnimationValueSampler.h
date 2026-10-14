#pragma once
/// @file

#include <optional>
#include <string>
#include <utility>

#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationValueMath.h"
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

}  // namespace donner::svg::components
