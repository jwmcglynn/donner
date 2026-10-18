#pragma once
/// @file

#include <string>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

namespace donner::svg::components {

/// A sampled motion transform with its effective composition mode.
struct SampledAnimationMotion {
  std::string transform;  ///< Supplemental translation and rotation, empty for invalid motion.
  bool additive = false;  ///< Whether to preserve lower motion layers.
};

/** Sample the supplemental transform for a motion animation.
 * @param registry Document storage.
 * @param entity Motion animation element.
 * @param motion Authored motion attributes.
 * @param progress Normalized simple time.
 * @param iteration Completed repeat count.
 * @param budget Shared per-frame geometry budget.
 * @return Sampled transform and composition mode; an empty transform denotes invalid motion.
 */
SampledAnimationMotion SampleAnimationMotion(Registry& registry, Entity entity,
                                             const AnimateMotionComponent& motion, double progress,
                                             double iteration, AnimationResourceBudget& budget);

}  // namespace donner::svg::components
