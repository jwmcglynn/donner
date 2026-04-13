#pragma once
/// @file

#include <cstdint>

namespace donner::svg::compositor {

/**
 * ECS component attached to entities that are assigned to a compositor layer.
 *
 * This component is attached lazily by `CompositorController::promoteEntity()` and removed by
 * `demoteEntity()`. Entities without this component are rendered into the root layer.
 *
 * @ingroup ecs_components
 */
struct LayerMembershipComponent {
  /// Unique identifier for the compositor layer this entity belongs to.
  uint32_t layerId = 0;
};

}  // namespace donner::svg::compositor
