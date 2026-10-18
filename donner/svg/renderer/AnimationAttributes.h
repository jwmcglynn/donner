#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/// Restore presentation-only paint/filter attributes before sampling the next frame.
/// @param registry Document storage.
/// @param target Restore only this entity, or all entities when null.
void RestoreAnimationAttributes(Registry& registry, Entity target = entt::null);

/// Apply sampled paint/filter attributes with bounded restoration storage.
/// @param registry Document storage.
/// @return False if restoration storage exceeds the animation output budget.
bool ApplyAnimationAttributes(Registry& registry);

}  // namespace donner::svg::components
