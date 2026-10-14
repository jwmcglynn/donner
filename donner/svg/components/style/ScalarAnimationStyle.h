#pragma once
/// @file

#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
class StyleResourceBudget;

/** Resolve scalar animation stacks and resource opacities in the final instance cascade.
 * @param registry Document storage with write access held.
 * @param dataEntity Authored data source, including for shadow instances.
 * @param parent Completed parent style, or null for the root.
 * @param properties Locally cascaded properties, updated at animation priority.
 * @param budget Shared style work budget.
 * @param warnings Receives invalid resource-style diagnostics.
 * @return Canonical resource opacity values for this instance.
 */
ComputedResourceOpacities ApplyScalarAnimationStyles(Registry& registry, Entity dataEntity,
                                                     const ComputedStyleComponent* parent,
                                                     PropertyRegistry& properties,
                                                     StyleResourceBudget& budget,
                                                     ParseWarningSink& warnings);

}  // namespace donner::svg::components
