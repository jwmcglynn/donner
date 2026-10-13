#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

class AnimationResourceBudget;

/// Admit geometry restoration storage before applying any presentation mutation.
/// @param registry Document storage with sampled animation values.
/// @param budget Shared output and restoration allocation envelope.
/// @return False if any geometry backup would exceed the envelope.
bool ReserveAnimationGeometryBackups(Registry& registry, AnimationResourceBudget& budget);

/// Restore the target's base values before a typed DOM setter modifies them.
/// @param registry Document storage with write access held.
/// @param entity Target of the impending authored mutation.
void RestoreAnimationPresentationForMutation(Registry& registry, Entity entity);

/// Sample animation and apply reversible style, geometry, and transform presentation values.
/// @param registry Document storage, held with write access by render preparation.
void ApplyAnimationPresentation(Registry& registry);

}  // namespace donner::svg::components
