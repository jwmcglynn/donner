#pragma once
/// @file

#include <memory>
#include <string>
#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueMath.h"
#include "donner/svg/components/animation/ContextualLengthRecipe.h"

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
 * @param budget Shared budget for resolving inherited coordinate units.
 * @return The target's current unit and color context.
 */
AnimationValueContext AnimationContextFor(Registry& registry, Entity target,
                                          std::string_view attribute,
                                          AnimationResourceBudget& budget);

/// Resolve typed, animated and CSS path data using the rendering cascade.
/// @param registry Document storage. @param target Path element. @param budget Shared admission.
/// @return Owning path data, or nullopt on admission refusal.
std::optional<RcString> CascadedAnimationPathData(Registry& registry, Entity target,
                                                  AnimationResourceBudget& budget);

/** Read the underlying presentation value, including lower animation layers.
 * @param registry Document storage.
 * @param target Animation target.
 * @param attribute Animated attribute name.
 * @param context Resolved target context.
 * @param budget Shared source budget, charged before any potentially large copy.
 * @return An owning serialized value (possibly empty), or nullopt when unavailable/refused.
 */

std::optional<std::string> UnderlyingAnimationValue(Registry& registry, Entity target,
                                                    std::string_view attribute,
                                                    const AnimationValueContext& context,
                                                    AnimationResourceBudget& budget);

/** Read the authored or styled base value without a lower animation override.
 * @param registry Document storage.
 * @param target Animation target.
 * @param attribute Animated attribute name.
 * @param context Resolved target context.
 * @param budget Shared source budget.
 * @return Owning serialized base value, or nullopt when unavailable/refused.
 */
std::optional<std::string> BaseAnimationValue(Registry& registry, Entity target,
                                              std::string_view attribute,
                                              const AnimationValueContext& context,
                                              AnimationResourceBudget& budget);

/// Find an owning contextual recipe, following a shadow entity to its authored source.
/// @param registry Document storage. @param target Target instance. @param attribute Length field.
std::shared_ptr<const ContextualLengthRecipe> ContextualLengthFor(Registry& registry, Entity target,
                                                                  std::string_view attribute);

/// Admit and evaluate one contextual field using the shared frame preparation budget.
/// @param registry Document storage. @param target Target instance. @param attribute Length field.
/// @param underlying Authored or typed base. @param context Referencing instance's units.
/// @return Sampled length, or nullopt for absent, invalid or over-budget animation.
std::optional<Lengthd> ResolveContextualLength(Registry& registry, Entity target,
                                               std::string_view attribute, Lengthd underlying,
                                               const AnimationValueContext& context);

}  // namespace donner::svg::components
