#pragma once
/// @file

#include <cstdint>

namespace donner::svg::compositor {

/**
 * Resolved-layer ECS component written by `LayerResolver` each frame; must
 * not be hand-edited. This mirrors the `StyleComponent` →
 * `ComputedStyleComponent` split in Donner's CSS engine: author-layer hints
 * live in `CompositorHintComponent`, and the resolver collapses them into
 * this computed component.
 *
 * Entities without this component render into the root layer (implicit
 * `layerId == 0`).
 *
 * @ingroup ecs_components
 */
struct ComputedLayerAssignmentComponent {
  /// Unique identifier for the compositor layer this entity belongs to. 0 is the root layer.
  uint32_t layerId = 0;
};

}  // namespace donner::svg::compositor
