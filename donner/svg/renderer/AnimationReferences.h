#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/// Restore authored resource references before rebuilding their presentation trees.
/// @param registry Document storage with write access held.
/// @param target One target, or null to restore the whole document.
void RestoreAnimationReferences(Registry& registry, Entity target = entt::null);

/// Invalidate derived resources only when the final sampled reference changed.
/// Call during a full rebuild, after old shadow instances have been torn down.
/// @param registry Document storage with write access held.
void FinalizeAnimationReferences(Registry& registry);

/// Apply sampled resource references before shadow-tree expansion and resource loading.
/// @param registry Document storage with sampled animation values.
/// @return False if restoration storage exceeds the shared animation budget.
bool ApplyAnimationReferences(Registry& registry);

}  // namespace donner::svg::components
