#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
class AnimationResourceBudget;

/** Sample paint and color stacks with the final instance's parent and current color.
 * @param animationBudget Contextual animation allowance for a serial cascade, or null for static
 * content.
 * @param registry Target document.
 * @param source Source element holding animation recipes.
 * @param parent Final computed parent, or null for the root.
 * @param inheritPaint Whether the parent permits fill/stroke inheritance.
 * @param properties Local cascade, updated only by admitted, valid animation values.
 */
void ApplyPaintAnimationStyles(const Registry& registry, AnimationResourceBudget* animationBudget,
                               Entity source, const ComputedStyleComponent* parent,
                               bool inheritPaint, PropertyRegistry& properties);

}  // namespace donner::svg::components
