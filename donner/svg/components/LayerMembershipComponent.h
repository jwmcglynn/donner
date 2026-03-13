#pragma once
/// @file

#include <cstdint>

namespace donner::svg::components {

/**
 * Maps an entity to the compositing layer it belongs to. Attached to every entity
 * in the render tree during layer decomposition.
 */
struct LayerMembershipComponent {
  /// Index into the LayerDecompositionResult::layers vector.
  uint32_t layerId = 0;
};

}  // namespace donner::svg::components
