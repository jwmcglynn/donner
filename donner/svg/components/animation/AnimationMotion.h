#pragma once
/// @file

#include <string>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

namespace donner::svg::components {

/** Sample the supplemental transform for a motion animation.
 * @param registry Document storage.
 * @param entity Motion animation element.
 * @param motion Authored motion attributes.
 * @param progress Normalized simple time.
 * @param iteration Completed repeat count.
 * @param budget Shared per-frame geometry budget.
 * @return Serialized translation/rotation, or empty for unresolved or invalid motion.
 */
std::string SampleAnimationMotion(Registry& registry, Entity entity,
                                  const AnimateMotionComponent& motion, double progress,
                                  double iteration, AnimationResourceBudget& budget);

}  // namespace donner::svg::components
