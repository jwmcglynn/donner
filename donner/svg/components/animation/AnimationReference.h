#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

namespace donner::svg::components {

/** Resolve an attached animation reference with bounded document traversal.
 * @param registry Document storage.
 * @param id Referenced ID, without a fragment prefix.
 * @param budget Shared sample or event allowance. Rejection is sticky.
 * @return Attached entity, or null for a missing ID or rejected lookup. Inspect budget.rejected()
 * to distinguish exhaustion from an ordinary missing reference.
 */
Entity FindAnimationReference(Registry& registry, std::string_view id,
                              AnimationResourceBudget& budget);

}  // namespace donner::svg::components
