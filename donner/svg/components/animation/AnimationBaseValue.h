#pragma once
/// @file

#include <string>
#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueMath.h"

namespace donner::svg::components {

/** Read the current typed resource reference, falling back to attributes for other elements.
 * @param registry Document storage.
 * @param target Reference-bearing element.
 * @return Owning reference value, or nullopt when unset.
 */
std::optional<RcString> AnimationReferenceValue(Registry& registry, Entity target);

/** Resolve units and colors for an animation target.
 * @param registry Document storage.
 * @param target Animation target.
 * @param attribute Animated attribute name.
 * @return The target's current unit and color context.
 */
AnimationValueContext AnimationContextFor(Registry& registry, Entity target,
                                          std::string_view attribute);

/** Read the underlying presentation value, including lower animation layers.
 * @param registry Document storage.
 * @param target Animation target.
 * @param attribute Animated attribute name.
 * @param context Resolved target context.
 * @param budget Shared source budget, charged before any potentially large copy.
 * @return An owning serialized value, empty when unavailable.
 */
std::string UnderlyingAnimationValue(Registry& registry, Entity target, std::string_view attribute,
                                     const AnimationValueContext& context,
                                     AnimationResourceBudget& budget);

}  // namespace donner::svg::components
